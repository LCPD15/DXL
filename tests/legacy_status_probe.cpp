#include "../src/common/IpcClient.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc,char** argv) {
    if(argc<2) return 2;
    const DWORD target=DWORD(strtoul(argv[1],nullptr,10));
    const unsigned seconds=argc>2?unsigned(strtoul(argv[2],nullptr,10)):30;
    const bool cycle=argc>3&&strcmp(argv[3],"--cycle")==0;
    const unsigned expectedApi=argc>4?unsigned(strtoul(argv[4],nullptr,10)):unsigned(DXL::Ipc::GraphicsApi::OpenGL);
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,target); if(!process)return 3;
    setvbuf(stdout,nullptr,_IONBF,0);
    DXL::StatusView view; DXL::Ipc::Status last{}; bool disabled=false,enabled=false;
    bool sawOff=false,offStable=true; uint64_t offNr=0;
    unsigned samples=0; ULONGLONG disabledAt=0; const ULONGLONG start=GetTickCount64();
    while(GetTickCount64()-start<ULONGLONG(seconds)*1000) {
        const auto elapsed=GetTickCount64()-start;
        if(cycle&&!disabled&&last.nrEvaluateCount>=60) {
            disabled=DXL::SendCommand(target,DXL::Ipc::CommandId::SetEnabled,0);
            if(disabled)disabledAt=elapsed;
        }
        if(cycle&&disabled&&elapsed>=disabledAt+4000&&!enabled) enabled=DXL::SendCommand(target,DXL::Ipc::CommandId::SetEnabled,1);
        if(!view.IsOpen())view.Open(target);
        if(view.Read(last)) {
            ++samples;
            if(cycle&&disabled&&!enabled&&!last.masterEnabled) {
                if(!sawOff){sawOff=true;offNr=last.nrEvaluateCount;}
                else if(last.nrEvaluateCount!=offNr)offStable=false;
            }
            printf("{\"elapsedMs\":%llu,\"pid\":%lu,\"api\":%u,\"master\":%u,\"nrState\":%u,\"width\":%u,\"height\":%u,\"presents\":%llu,\"nr\":%llu,\"failures\":%llu,\"skips\":%llu,\"frameMs\":%.3f,\"route\":%u,\"motion\":%u,\"opticalMs\":%.3f,\"stalls\":%u}\n",
                elapsed,target,last.api,last.masterEnabled,last.nrState,last.outputWidth,last.outputHeight,
                last.presentCount,last.nrEvaluateCount,last.nrEvaluateFailures,last.nrSkippedFrames,last.frameMs,
                last.nrRoute,last.nrMotionSource,last.nrOpticalFlowMs,last.stallCount);
        }
        if(WaitForSingleObject(process,500)==WAIT_OBJECT_0)break;
    }
    CloseHandle(process);
    const bool cycleVerified=disabled&&enabled&&sawOff&&offStable&&last.nrEvaluateCount>offNr;
    const bool passed=samples>5&&last.api==expectedApi&&last.nrEvaluateCount>=30&&last.nrEvaluateFailures==0&&(!cycle||cycleVerified);
    printf("LEGACY_NR_RESULT passed=%u samples=%u nr=%llu failures=%llu cycled=%u offStable=%u\n",passed?1u:0u,samples,last.nrEvaluateCount,last.nrEvaluateFailures,cycleVerified?1u:0u,offStable?1u:0u);
    return passed?0:1;
}
