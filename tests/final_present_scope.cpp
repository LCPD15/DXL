#include "FinalPresentScope.h"
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
using namespace DXL;
static void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
struct Fixture {
    FinalPresentationPolicy policy;
    std::vector<std::string> events;
    unsigned input = 0, final = 0, ui = 0;
    uint64_t now = 100;
    void Present(uintptr_t native, bool queue, bool effectValid, const std::function<void()>& forward = {}, bool test = false) {
        FinalPresentScope scope;
        if (test) return;
        policy.Begin(true, now);
        if (scope.AdvanceInputOnce()) { ++input; scope.SetCloseOnEscape(true); events.push_back("input"); }
        Check(scope.CloseOnEscape(), "Nested physical callback lost the input snapshot");
        const bool accepted = !native || scope.EnterNative(native);
        if (accepted) {
            const bool fallback = !native && scope.Outer() && policy.ProxyUiFallback(now);
            if (native && queue) {
                policy.NativeReady(now);
                if (!scope.UpstreamUiDrawn()) {
                    if (effectValid) { ++final; events.push_back("final"); }
                    ++ui; events.push_back("ui");
                }
            } else if (fallback && queue) {
                scope.MarkUpstreamUi(); ++ui; events.push_back("fallback-ui");
            }
        }
        if (forward) forward();
    }
};
int main() try {
    {
        Fixture f;f.Present(0,false,true,[&]{f.Present(1,true,true,[&]{f.Present(1,true,true);});});
        Check(f.input==1&&f.final==1&&f.ui==1,"Present1 forwarding must draw exactly once");
        Check(f.events==std::vector<std::string>{"input","final","ui"},"Native output must precede UI");
    }
    {
        Fixture f;f.Present(0,false,true,[&]{
            f.Present(1,true,true,[&]{f.Present(1,true,true);});
            f.Present(1,true,true,[&]{f.Present(1,true,true);});
            f.Present(2,true,true);
        });
        Check(f.input==1&&f.final==3&&f.ui==3,"Sibling physical outputs were skipped or forwarded twice");
        Check(f.events==std::vector<std::string>{"input","final","ui","final","ui","final","ui"},"Sibling output ordering");
    }
    {
        Fixture f;f.Present(0,false,false,[&]{f.Present(1,true,false);});
        Check(f.input==1&&f.final==0&&f.ui==1,"Invalid effect suppressed a valid native UI");
    }
    {
        Fixture f;f.Present(0,true,true); // Initial discovery defers upstream UI.
        Check(f.ui==0,"Unknown wrapper drew UI before initial native discovery");
        f.now=1200;f.Present(0,true,true);Check(f.ui==1,"Unsupported wrapper lost its only controls");
        f.Present(0,true,true,[&]{f.Present(1,true,true);f.Present(1,true,true);});
        Check(f.ui==2&&f.final==0,"Recovery modified or duplicated an upstream fallback overlay");
        f.now=1210;f.Present(0,true,true,[&]{f.Present(1,true,true);f.Present(1,true,true);});
        Check(f.ui==4&&f.final==2,"Native final/UI did not recover after fallback discovery");
        f.now=2300;f.Present(0,true,true,[&]{f.Present(1,false,true);});
        Check(f.ui==5,"Lost native queue suppressed the controls fallback");
    }
    {
        Fixture f;f.Present(1,true,true,{},true);Check(!f.input&&!f.final&&!f.ui,"Present TEST must not process input or rendering");
        f.Present(1,true,true,[&]{f.Present(1,true,true,{},true);});
        Check(f.input==1&&f.final==1&&f.ui==1,"Nested TEST affected native physical work");
    }
    {
        // The old no-post route has already drawn the proxy overlay. A player
        // enables an effect in that overlay before its native child executes.
        // This first activation must be suppressed just like fallback recovery.
        FinalPresentScope wrapper;Check(wrapper.AdvanceInputOnce(),"Activation input");
        wrapper.MarkUpstreamUi();
        { FinalPresentScope native;Check(native.EnterNative(1),"Activation native identity");
          Check(native.UpstreamUiDrawn()&&!native.AdvanceInputOnce(),"Mid-overlay activation lost upstream UI state"); }
        { FinalPresentScope sibling;Check(sibling.EnterNative(1)&&sibling.UpstreamUiDrawn(),"Mid-overlay activation affected sibling output"); }
    }
    puts("PASS: physical Present ancestry/siblings, one input snapshot, final-before-UI, invalid effects, TEST and proxy fallback/recovery.");
    return 0;
} catch(const std::exception& e) { std::printf("FAIL: %s\n",e.what()); return 1; }
