#include <windows.h>
#include <shlobj.h>
#include <conio.h>

#include <cwchar>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {
[[noreturn]] void WinError(const char* operation, DWORD error = GetLastError()) {
	throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

struct CloseWinHandle {
	void operator()(HANDLE value) const noexcept {
		if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
	}
};
using Handle = std::unique_ptr<void, CloseWinHandle>;

// Windows CRT quoting: backslashes before quotes and the closing quote double.
std::wstring Quote(const std::wstring& argument) {
	std::wstring result = L"\"";
	size_t slashes = 0;
	for (wchar_t ch : argument) {
		if (ch == L'\\') {
			++slashes;
			continue;
		}
		result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		slashes = 0;
		result += ch;
	}
	result.append(slashes * 2, L'\\');
	return result + L'"';
}

struct Process {
	Handle handle;
	DWORD pid;
};

Process StartProcess(const fs::path& executable, std::wstring command,
	DWORD_PTR attribute, HANDLE value, DWORD flags = 0) {
	if (command.size() >= 32767) throw std::runtime_error("Command line exceeds the Windows limit.");
	const auto directory = executable.parent_path().wstring();
	SIZE_T size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
	if (!size) WinError("Size process attributes");
	std::vector<unsigned char> storage(size);
	auto rawAttributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
	if (!InitializeProcThreadAttributeList(rawAttributes, 1, 0, &size))
		WinError("Initialize process attributes");
	std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)>
		attributes(rawAttributes, DeleteProcThreadAttributeList);
	if (!UpdateProcThreadAttribute(attributes.get(), 0, attribute, &value, sizeof(value), nullptr, nullptr))
		WinError("Set process attribute");
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.lpAttributeList = attributes.get();
	PROCESS_INFORMATION info{};
	if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
		EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | flags,
		nullptr, directory.c_str(), &startup.StartupInfo, &info))
		WinError("CreateProcessW");
	CloseHandle(info.hThread);
	return {Handle(info.hProcess), info.dwProcessId};
}

Handle CreateParentJob() {
	Handle job(CreateJobObjectW(nullptr, nullptr));
	if (!job) WinError("Create parent cleanup job");
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	// Closing our last job handle cleans up the dummy even if we crash.
	// The game must NOT join this job, so closing the launcher won't kill it.
	limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
	if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		WinError("Configure parent cleanup job");
	return job;
}

struct ParentProcess {
	fs::path path;
	fs::path temporaryDirectory;
	Process process{};
	Handle job;

	~ParentProcess() {
		job.reset();
		// The process must release its executable before we delete the copy.
		if (process.handle) {
			if (WaitForSingleObject(process.handle.get(), INFINITE) == WAIT_FAILED)
				std::cerr << "Failed to wait for parent cleanup: " << GetLastError() << '\n';
			process.handle.reset();
		}
		if (temporaryDirectory.empty()) return;
		SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
		std::error_code error;
		fs::remove(path, error);
		if (error) std::cerr << "Failed to delete temporary Games.exe: " << error.message() << '\n';
		fs::remove(temporaryDirectory, error);
		if (error) std::cerr << "Failed to remove temporary directory: " << error.message() << '\n';
	}
};

fs::path LauncherDirectory() {
	std::wstring path(32768, L'\0');
	DWORD length = static_cast<DWORD>(path.size());
	if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, path.data(), &length)) WinError("Get launcher path");
	path.resize(length);
	return fs::path(path).parent_path();
}

fs::path ExistingFile(const fs::path& path) {
	if (!fs::is_regular_file(path)) throw std::runtime_error("Executable not found: " + path.string());
	return fs::canonical(path);
}

fs::path SdkLogPath() {
	PWSTR raw = nullptr;
	const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &raw);
	std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
	if (FAILED(result)) WinError("Locate SDK log", static_cast<DWORD>(result));
	return fs::path(folder.get()) / L"Gryphline" / L"Endfield" / L"sdklogs" / L"OneChannelLC.log";
}

struct LaunchLog {
	fs::path path;
	BY_HANDLE_FILE_INFORMATION file{};
	LONGLONG offset = 0;
	std::string pending;
	bool matchedParent = false;
};

// PID 0 snapshots the existing log before launch. Only subsequent complete
// lines can confirm this parent's verification; file rotation resets state.
bool ReadLaunchCheck(LaunchLog& log, DWORD parentPid) {
	Handle file(CreateFileW(log.path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
	if (file.get() == INVALID_HANDLE_VALUE) {
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
			(parentPid && error == ERROR_SHARING_VIOLATION))
			return false;
		WinError("Read SDK log", error);
	}
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(file.get(), &info)) WinError("Inspect SDK log");
	const auto size = (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	if (!parentPid || info.dwVolumeSerialNumber != log.file.dwVolumeSerialNumber ||
		info.nFileIndexHigh != log.file.nFileIndexHigh || info.nFileIndexLow != log.file.nFileIndexLow ||
		size < log.offset) {
		log.offset = parentPid ? 0 : size;
		log.pending.clear();
		log.matchedParent = false;
	}
	log.file = info;
	if (!parentPid) return false;
	LARGE_INTEGER position{};
	position.QuadPart = log.offset;
	if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) WinError("Seek SDK log");
	char buffer[16384];
	DWORD count = 0;
	if (!ReadFile(file.get(), buffer, sizeof(buffer), &count, nullptr)) WinError("Read SDK log");
	log.offset += count;
	log.pending.append(buffer, count);
	const std::string parentMarker = "Parent process check success:";
	const auto expectedPid = std::to_string(parentPid);
	size_t end = 0;
	while ((end = log.pending.find('\n')) != std::string::npos) {
		const auto line = log.pending.substr(0, end);
		log.pending.erase(0, end + 1);
		const auto match = line.find(parentMarker);
		if (match != std::string::npos) {
			const auto pidStart = match + parentMarker.size();
			const auto pidEnd = line.find_first_not_of("0123456789", pidStart);
			log.matchedParent = line.substr(pidStart, pidEnd - pidStart) == expectedPid;
		}
		if (log.matchedParent && line.find("Record check result:") != std::string::npos) {
			if (line.find("\"msg\":{\"code\":0,") == std::string::npos)
				throw std::runtime_error("SDK launch check failed.");
			return true;
		}
	}
	if (log.pending.size() > 65536) log.pending.clear();
	return false;
}

void WaitForLaunchCheck(LaunchLog& log, const Process& child, const Process& parent) {
	const auto deadline = GetTickCount64() + 180000;
	HANDLE processes[]{child.handle.get(), parent.handle.get()};
	while (GetTickCount64() < deadline) {
		if (ReadLaunchCheck(log, parent.pid)) return;
		const auto wait = WaitForMultipleObjects(2, processes, FALSE, 250);
		if (wait == WAIT_OBJECT_0) throw std::runtime_error("Game exited before parent verification.");
		if (wait == WAIT_OBJECT_0 + 1) throw std::runtime_error("Parent exited before verification.");
		if (wait == WAIT_FAILED) WinError("Wait for parent verification");
	}
	throw std::runtime_error("Timed out waiting for parent verification.");
}

int Run(int argc, wchar_t** argv) {
	const auto directory = LauncherDirectory();
	fs::path game = directory / L"Endfield.exe";
	fs::path parentSource = directory / L"PlatformProcess.exe";
	std::wstring arguments;
	for (int i = 1; i < argc; ++i) {
		const std::wstring option = argv[i];
		if (option == L"--help" || option == L"-h") {
			std::wcout << L"anef_dummy_launcher [--game PATH] [--parent PATH] [-- ARGS...]\n";
			return 0;
		}
		if (option == L"--") {
			for (++i; i < argc; ++i) arguments += L" " + Quote(argv[i]);
			break;
		}
		if (option == L"--game" || option == L"--parent") {
			if (++i == argc) throw std::runtime_error("Missing path after --game or --parent.");
			(option == L"--game" ? game : parentSource) = argv[i];
			continue;
		}
		throw std::runtime_error("Unknown option. Use --help; put game arguments after --.");
	}

	game = ExistingFile(game);
	parentSource = ExistingFile(parentSource);
	ParentProcess parent;
	parent.path = parentSource;
	if (_wcsicmp(parentSource.filename().c_str(), L"Games.exe") != 0) {
		auto temporaryDirectory = fs::temp_directory_path() /
			(L"anef_dummy_launcher-" + std::to_wstring(GetCurrentProcessId()));
		parent.path = temporaryDirectory / L"Games.exe";
		if (!fs::create_directory(temporaryDirectory))
			throw std::runtime_error("Temporary parent directory already exists.");
		parent.temporaryDirectory.swap(temporaryDirectory);
		fs::copy_file(parentSource, parent.path);
	}
	const auto command = Quote(game.wstring()) + arguments;
	std::wcout << L"Parent: " << parent.path.wstring() << L'\n'
		<< L"Game:   " << command << L"\nDirectory: " << game.parent_path().wstring() << L'\n';

	LaunchLog log{SdkLogPath()};
	ReadLaunchCheck(log, 0);
	parent.job = CreateParentJob();
	parent.process = StartProcess(parent.path, Quote(parent.path.wstring()),
		PROC_THREAD_ATTRIBUTE_JOB_LIST, parent.job.get(), CREATE_SUSPENDED);
	std::wcout << L"Parent PID: " << parent.process.pid << L'\n';
	auto child = StartProcess(game, command, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, parent.process.handle.get());
	std::wcout << L"Game PID: " << child.pid << L'\n';
	WaitForLaunchCheck(log, child, parent.process);
	std::wcout << L"Parent verified.\n";
	return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
	std::wcout << std::unitbuf;
	try { return Run(argc, argv); }
	catch (const std::exception& error) {
		std::cerr << "Error: " << error.what() << '\n';
		std::cerr << "Press any key to continue..." << std::flush;
		_getwch();
		return 1;
	}
}
