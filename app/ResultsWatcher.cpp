// Observe this process's native 1.16.1 score/menu transition. No game-memory writes.
// BWAPI 4.4.0 documents these fields in BW/Offsets.h and enums in BW/MenuPosition.h.
#include <windows.h>
#include <cstdint>
#include <fstream>
static LONG started = 0;
static bool returnMenu(unsigned menu) {
    switch(menu) {case 0:case 2:case 3:case 6:case 10:case 11:case 12:case 22:case 23:case 24:return true;}
    return false;
}
static DWORD WINAPI watch(LPVOID) {
    bool seen = false;
    unsigned priorMode=~0u,priorMenu=~0u;
    int returned=0;
    for(;;) {
        unsigned mode=*reinterpret_cast<volatile std::uint16_t*>(0x00596904);
        unsigned menu=*reinterpret_cast<volatile std::uint32_t*>(0x006D11BC);
        if(mode==4 && menu>=14 && menu<=19)seen=true;
        returned=seen && mode==4 && returnMenu(menu)?returned+1:0;
        if(mode!=priorMode || menu!=priorMenu) {
            std::ofstream("bwapi-data/write/results-watch.tmp")<<"{\"game_mode\":"<<mode
                <<",\"menu_mode\":"<<menu<<",\"seen_scores\":"<<(seen?"true":"false")<<"}";
            MoveFileExA("bwapi-data/write/results-watch.tmp","bwapi-data/write/results-watch.json",MOVEFILE_REPLACE_EXISTING);
            priorMode=mode;priorMenu=menu;
        }
        if(returned>=2) {
            std::ofstream("bwapi-data/write/results-dismissed.tmp")<<"{\"reason\":\"score_ok\",\"game_mode\":"<<mode<<",\"menu_mode\":"<<menu<<"}";
            MoveFileExA("bwapi-data/write/results-dismissed.tmp","bwapi-data/write/results-dismissed.json",MOVEFILE_REPLACE_EXISTING);
            return 0;
        }
        Sleep(150);
    }
}
extern "C" __declspec(dllexport) void __cdecl StartResultsWatcher() {
    if(InterlockedCompareExchange(&started,1,0))return;
    // AIModule DLLs can be unloaded after onEnd. Keep this independent reporter
    // alive until the game process exits, so no callback can point at freed code.
    HMODULE module=nullptr;
    if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCSTR>(&StartResultsWatcher),&module))return;
    HANDLE thread=CreateThread(nullptr,0,watch,nullptr,0,nullptr);
    if(thread)CloseHandle(thread);
}
