#pragma once

// 读 core 状态块、发命令 —— UI 宿主和命令行注入器都要用，所以放在 common 下。
// 只有客户端一侧的代码：core 侧的发布逻辑在 core.cpp 里。

#include <windows.h>
#include <cstdio>

#include "IpcProtocol.h"

namespace DXL {

// core 发布的状态块。seqlock 读：读到奇数或前后 sequence 不一致就重试。
class StatusView {
public:
	~StatusView() { Close(); }

	StatusView() = default;
	StatusView(const StatusView&) = delete;
	StatusView& operator=(const StatusView&) = delete;

	bool Open(DWORD pid) {
		Close();
		wchar_t name[128]{};
		_snwprintf_s(name, _TRUNCATE, L"%s.%lu", Ipc::STATUS_MEMORY_BASE, pid);
		_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
		if (!_mapping) return false;
		_status = static_cast<const Ipc::Status*>(MapViewOfFile(
			_mapping, FILE_MAP_READ, 0, 0, sizeof(Ipc::Status)));
		if (!_status) {
			Close();
			return false;
		}
		_pid = pid;
		return true;
	}

	void Close() {
		if (_status) {
			UnmapViewOfFile(_status);
			_status = nullptr;
		}
		if (_mapping) {
			CloseHandle(_mapping);
			_mapping = nullptr;
		}
		_pid = 0;
	}

	bool IsOpen() const { return _status != nullptr; }
	DWORD Pid() const { return _pid; }

	bool Read(Ipc::Status& out) const {
		if (!_status) return false;
		for (int attempt = 0; attempt < 8; ++attempt) {
			const uint32_t before = _status->sequence;
			if (before & 1u) continue;          // core 正在写
			MemoryBarrier();
			out = *_status;
			MemoryBarrier();
			if (_status->sequence == before && out.magic == Ipc::MAGIC) return true;
		}
		return false;
	}

private:
	HANDLE _mapping = nullptr;
	const Ipc::Status* _status = nullptr;
	DWORD _pid = 0;
};

// 一次请求/应答。core 侧每次只处理一个连接，所以这里连上就写、写完就读。
inline bool SendCommand(DWORD pid, Ipc::CommandId id, uint32_t arg0 = 0) {
	wchar_t name[128]{};
	_snwprintf_s(name, _TRUNCATE, L"%s.%lu", Ipc::COMMAND_PIPE_BASE, pid);

	HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_EXISTING, 0, nullptr);
	if (pipe == INVALID_HANDLE_VALUE) {
		// core 可能正好在处理上一个连接，等一下再试一次
		if (GetLastError() != ERROR_PIPE_BUSY ||
			!WaitNamedPipeW(name, 1000)) {
			return false;
		}
		pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			OPEN_EXISTING, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) return false;
	}

	DWORD mode = PIPE_READMODE_MESSAGE;
	SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

	const Ipc::Command command{
		Ipc::MAGIC, Ipc::VERSION, static_cast<uint32_t>(id), arg0 };
	DWORD transferred = 0;
	bool ok = WriteFile(pipe, &command, sizeof(command), &transferred, nullptr) &&
		transferred == sizeof(command);
	if (ok) {
		Ipc::CommandReply reply{};
		ok = ReadFile(pipe, &reply, sizeof(reply), &transferred, nullptr) &&
			transferred == sizeof(reply) && reply.accepted != 0;
	}
	CloseHandle(pipe);
	return ok;
}

}  // namespace DXL
