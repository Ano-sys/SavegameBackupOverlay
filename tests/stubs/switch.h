#pragma once

#include <cstdint>
#include <chrono>
#include <thread>

using u64 = uint64_t;
using s64 = int64_t;
using Result = uint32_t;
inline void svcSleepThread(int64_t nanoseconds) { std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds)); }
struct AccountUid {
    u64 uid[2];
};

struct SetSysDeviceNickName {
    char nickname[0x80];
};
Result setInitialize();
void setExit();
Result setGetDeviceNickname(SetSysDeviceNickName*);
Result setsysInitialize();
void setsysExit();
Result setsysGetDeviceNickname(SetSysDeviceNickName*);

enum { Module_Libnx = 345,
    LibnxError_OutOfMemory = 2 };
enum BsdServiceType { BsdServiceType_Auto = 3 };
struct SocketInitConfig {
    uint32_t tcp_tx_buf_size, tcp_rx_buf_size, tcp_tx_buf_max_size, tcp_rx_buf_max_size;
    uint32_t udp_tx_buf_size, udp_rx_buf_size, sb_efficiency, num_bsd_sessions;
    BsdServiceType bsd_service_type;
};

Result socketInitialize(const SocketInitConfig* settings);
Result socketGetLastResult();
void socketExit();

struct Thread {
    std::thread* native = nullptr;
    void (*entry)(void*) = nullptr;
    void* context = nullptr;
};
inline unsigned test_open_threads = 0;
inline Result test_thread_close_result = 0;
inline Result test_thread_create_result = 0;
inline Result test_thread_start_result = 0;
inline unsigned test_thread_creations = 0;

#define R_FAILED(result) ((result) != 0)
#define R_SUCCEEDED(result) ((result) == 0)
#define MAKERESULT(module, description) ((module) | ((description) << 9))

inline Result threadCreate(Thread* thread, void (*entry)(void*), void* context, void*, size_t, int, int) {
    ++test_thread_creations;
    if(test_thread_create_result) return test_thread_create_result;
    thread->entry = entry;
    thread->context = context;
    return 0;
}

inline Result threadStart(Thread* thread) {
    if(test_thread_start_result) return test_thread_start_result;
    thread->native = new std::thread(thread->entry, thread->context);
    ++test_open_threads;
    return 0;
}

inline Result threadWaitForExit(Thread* thread) {
    if(thread->native && thread->native->joinable()) thread->native->join();
    return 0;
}

inline Result threadClose(Thread* thread) {
    if(test_thread_close_result) return test_thread_close_result;
    if(thread->native) --test_open_threads;
    delete thread->native;
    thread->native = nullptr;
    return 0;
}

Result pmdmntInitialize();
void pmdmntExit();
Result pmdmntGetApplicationProcessId(u64* process_id);
Result pmdmntGetProgramId(u64* application_id, u64 process_id);
Result fsdevMountSaveData(const char* name, u64 application_id, AccountUid uid);
Result fsdevMountSaveDataReadOnly(const char* name, u64 application_id, AccountUid uid);
int fsdevUnmountDevice(const char* name);
Result fsdevCommitDevice(const char* name);

enum FsSaveDataSpaceId { FsSaveDataSpaceId_User = 1 };
enum { FsSaveDataType_Account = 1 };
struct FsSaveDataInfoReader {
    bool consumed = false;
};
struct FsSaveDataInfo {
    u64 save_data_id;
    uint8_t save_data_space_id, save_data_type;
    AccountUid uid;
    u64 application_id;
    uint16_t save_data_index;
    uint8_t save_data_rank;
};
struct FsSaveDataExtraData {
    s64 journal_size;
};
Result fsOpenSaveDataInfoReader(FsSaveDataInfoReader*, FsSaveDataSpaceId);
Result fsSaveDataInfoReaderRead(FsSaveDataInfoReader*, FsSaveDataInfo*, size_t, s64*);
void fsSaveDataInfoReaderClose(FsSaveDataInfoReader*);
Result fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(void*, size_t, FsSaveDataSpaceId, u64);
