#pragma once

// 自己实现的 NVSDK_NGX_Parameter。
//
// 为什么要自己实现：DLSSNR 走的是签名 snippet 自己的 Init_Ext / CreateFeature /
// EvaluateFeature，本来和 NGX core 无关。但 snippet **不导出**
// AllocateParameters（只导出 PopulateParameters_Impl），所以原来只能借 NGX core
// 的 `NVSDK_NGX_D3D12_AllocateParameters` 拿参数容器 —— 而那会顺带把 NGX core
// 初始化起来，把游戏自己的 NGX 会话弄坏（见 README 的硬约束）。
//
// NVSDK_NGX_Parameter 是个纯虚接口，没有任何 NGX 内部依赖，自己实现一份之后
// **DLSSNR 就能和游戏原生的 DLSS 共存**：我们完全不碰 NGX core。
//
// 实现上刻意不用 std 容器：这段代码跑在别人的游戏进程里，固定大小的数组 +
// 线性查找没有任何分配，最不容易出事。键的数量是几十个量级，线性查找足够。

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cstring>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

namespace DXL {

class NgxParameterBag final : public NVSDK_NGX_Parameter {
public:
	NgxParameterBag() noexcept {
		InitializeSRWLock(&_lock);
		InitializeSRWLock(&_missLock);
	}

	NgxParameterBag(const NgxParameterBag&) = delete;
	NgxParameterBag& operator=(const NgxParameterBag&) = delete;

	/* ---------------- Set ---------------- */

	void Set(const char* name, unsigned long long value) override {
		Store(name, Kind::U64, [&](Value& v) { v.u64 = value; });
	}
	void Set(const char* name, float value) override {
		Store(name, Kind::F32, [&](Value& v) { v.f32 = value; });
	}
	void Set(const char* name, double value) override {
		Store(name, Kind::F64, [&](Value& v) { v.f64 = value; });
	}
	void Set(const char* name, unsigned int value) override {
		Store(name, Kind::U32, [&](Value& v) { v.u32 = value; });
	}
	void Set(const char* name, int value) override {
		Store(name, Kind::I32, [&](Value& v) { v.i32 = value; });
	}
	void Set(const char* name, ID3D11Resource* value) override {
		Store(name, Kind::Pointer, [&](Value& v) { v.pointer = value; });
	}
	void Set(const char* name, ID3D12Resource* value) override {
		Store(name, Kind::Pointer, [&](Value& v) { v.pointer = value; });
	}
	void Set(const char* name, void* value) override {
		Store(name, Kind::Pointer, [&](Value& v) { v.pointer = value; });
	}

	/* ---------------- Get ---------------- */
	// 数值类型之间要能互转：NGX 自己的实现就是这么宽松的，snippet 完全可能
	// Set 成 unsigned int 再 Get 成 int，或者 Get 成 unsigned long long。
	// 严格按写入类型匹配会让它读不到我们设的值，而且是静默失败。

	NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override {
		return Read(name, [&](const Entry& e) {
			*out = (unsigned long long)AsDouble(e);
		});
	}
	NVSDK_NGX_Result Get(const char* name, float* out) const override {
		return Read(name, [&](const Entry& e) { *out = (float)AsDouble(e); });
	}
	NVSDK_NGX_Result Get(const char* name, double* out) const override {
		return Read(name, [&](const Entry& e) { *out = AsDouble(e); });
	}
	NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override {
		return Read(name, [&](const Entry& e) { *out = (unsigned int)AsDouble(e); });
	}
	NVSDK_NGX_Result Get(const char* name, int* out) const override {
		return Read(name, [&](const Entry& e) { *out = (int)AsDouble(e); });
	}
	NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override {
		return Read(name, [&](const Entry& e) {
			*out = static_cast<ID3D11Resource*>(e.value.pointer);
		});
	}
	NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override {
		return Read(name, [&](const Entry& e) {
			*out = static_cast<ID3D12Resource*>(e.value.pointer);
		});
	}
	NVSDK_NGX_Result Get(const char* name, void** out) const override {
		return Read(name, [&](const Entry& e) { *out = e.value.pointer; });
	}

	void Reset() override {
		AcquireSRWLockExclusive(&_lock);
		_count = 0;
		ReleaseSRWLockExclusive(&_lock);
	}

	uint32_t Count() const noexcept { return _count; }

	/* ---------------- snippet 到底想要什么（诊断） ---------------- */
	//
	// DLSSNR 没有公开头文件，我们那张参数表是逐个试出来的 —— 所以一定有它想要而我们
	// 没给的键。这里把**它问了、我们答不出来**的键名记下来。
	//
	// 这比猜键名强得多：`nvngx_dlssnr.dll` 自己会说它要什么。第一次在真实游戏的
	// evaluate 点上跑，画面变成一片灰（线性 HDR 喂给一个按最终画面设计的滤镜），
	// 当时能想到的下一步就是"它是不是有个 HDR / 曝光相关的键我们没设"——
	// 与其试键名，不如让它自己报。

	uint32_t MissCount() const noexcept {
		return _missCount;
	}
	// i < MissCount()。返回的指针指向内部缓冲，只在这个对象活着期间有效。
	const char* Miss(uint32_t i) const noexcept {
		return i < _missCount ? _misses[i] : "";
	}

	// 我们设了、但 snippet **从来没问过**的键。
	//
	// 这是上面那半边的镜像，而且是更早的警报：带点的那 16 个子矩形键名错了整整几个
	// 月都没人发现，因为"设了"看起来就像"生效了"。一个从来没被读过的键，要么名字错，
	// 要么这个 DLL 根本不认它 —— 两种都该知道。
	uint32_t UnreadCount() const noexcept {
		uint32_t n = 0;
		for (uint32_t i = 0; i < _count; ++i) {
			if (!_entries[i].read) ++n;
		}
		return n;
	}
	// 第 i 个"没被读过"的键名（i < UnreadCount()）
	const char* Unread(uint32_t i) const noexcept {
		uint32_t n = 0;
		for (uint32_t e = 0; e < _count; ++e) {
			if (_entries[e].read) continue;
			if (n++ == i) return _entries[e].key;
		}
		return "";
	}

private:
	// 键名都是 "DLSSNR.LocalStructureStrength" 这种量级，64 字节够用
	static constexpr size_t MAX_KEY = 64;
	static constexpr uint32_t MAX_ENTRIES = 256;

	enum class Kind : uint8_t { U64, F32, F64, U32, I32, Pointer };

	union Value {
		unsigned long long u64;
		float f32;
		double f64;
		unsigned int u32;
		int i32;
		void* pointer;
	};

	struct Entry {
		char key[MAX_KEY]{};
		Value value{};
		Kind kind = Kind::U32;
		// snippet 读过这个键没有。诊断用，见 UnreadCount()。
		// Read 是 const 的，所以要 mutable —— 这只是观测，不改变逻辑语义。
		mutable bool read = false;
	};

	static double AsDouble(const Entry& entry) noexcept {
		switch (entry.kind) {
		case Kind::U64: return (double)entry.value.u64;
		case Kind::F32: return (double)entry.value.f32;
		case Kind::F64: return entry.value.f64;
		case Kind::U32: return (double)entry.value.u32;
		case Kind::I32: return (double)entry.value.i32;
		case Kind::Pointer: break;
		}
		// 指针当数值读：NGX 有些键就是这么用的（把句柄塞在 u64 里）
		return (double)(unsigned long long)(uintptr_t)entry.value.pointer;
	}

	int Find(const char* name) const noexcept {
		if (!name) return -1;
		for (uint32_t i = 0; i < _count; ++i) {
			if (strcmp(_entries[i].key, name) == 0) return int(i);
		}
		return -1;
	}

	template <typename Assign>
	void Store(const char* name, Kind kind, Assign&& assign) noexcept {
		if (!name || !*name) return;
		AcquireSRWLockExclusive(&_lock);
		int index = Find(name);
		if (index < 0) {
			if (_count >= MAX_ENTRIES) {
				ReleaseSRWLockExclusive(&_lock);
				return;
			}
			index = int(_count++);
			strncpy_s(_entries[index].key, name, MAX_KEY - 1);
		}
		_entries[index].kind = kind;
		assign(_entries[index].value);
		ReleaseSRWLockExclusive(&_lock);
	}

	// 记下一个"问了但没有"的键。去重，满了就不再记。
	// 用**独立的锁**：Read 持的是共享锁，在里面升级成独占会死锁。
	void NoteMiss(const char* name) const noexcept {
		if (!name || !*name) return;
		AcquireSRWLockExclusive(&_missLock);
		for (uint32_t i = 0; i < _missCount; ++i) {
			if (strcmp(_misses[i], name) == 0) {
				ReleaseSRWLockExclusive(&_missLock);
				return;
			}
		}
		if (_missCount < MAX_MISSES) {
			strncpy_s(_misses[_missCount], name, MAX_KEY - 1);
			++_missCount;
		}
		ReleaseSRWLockExclusive(&_missLock);
	}

	template <typename Extract>
	NVSDK_NGX_Result Read(const char* name, Extract&& extract) const noexcept {
		AcquireSRWLockShared(&_lock);
		const int index = Find(name);
		if (index < 0) {
			ReleaseSRWLockShared(&_lock);
			// 没设过的键必须明确报 not found —— snippet 靠这个判断"用默认值"
			NoteMiss(name);
			return NVSDK_NGX_Result_FAIL_InvalidParameter;
		}
		_entries[index].read = true;
		extract(_entries[index]);
		ReleaseSRWLockShared(&_lock);
		return NVSDK_NGX_Result_Success;
	}

	// snippet 可能从它自己的线程读参数，用读写锁比互斥量更合适（读远多于写）
	mutable SRWLOCK _lock{};
	Entry _entries[MAX_ENTRIES]{};
	uint32_t _count = 0;

	static constexpr uint32_t MAX_MISSES = 64;
	mutable SRWLOCK _missLock{};
	mutable char _misses[MAX_MISSES][MAX_KEY]{};
	mutable uint32_t _missCount = 0;
};

}  // namespace DXL
