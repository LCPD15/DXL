#include "../src/common/SettingsReader.h"
#include <cstdio>
#include <stdexcept>
#include <limits>

using namespace DXL;
static void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static void Put(const std::filesystem::path& file, const std::string& text) {
    FILE* f = nullptr; Check(!_wfopen_s(&f,file.c_str(),L"wb") && f, "open fixture");
    const bool ok=fwrite(text.data(),1,text.size(),f)==text.size(); fclose(f); Check(ok,"write fixture");
}
int main() try {
    const auto root=std::filesystem::temp_directory_path() / (L"dxl-color-settings-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const std::string filename="夜景, {电影} LUT.png";
    Put(root/L"base.json","{\"masterEnabled\":true,\"dlss5Enable\":true,\"colorExposure\":3}");
    Put(root/L"game-a.params.json", "{\"masterEnabled\":false,\"colorExposureEnabled\":true,\"colorExposure\":-1.25,\"colorContrastEnabled\":true,\"colorContrast\":0,\"colorLutEnabled\":true,\"colorLutIntensity\":0.35,\"colorLutFile\":"+JsonScalar::Quote(filename)+",\"nrProcessingEnabled\":false}");
    Put(root/L"game-b.params.json","{\"colorSaturationEnabled\":true,\"colorSaturation\":1.75}");
    SettingsReader a,b; Check(a.Load(root/L"base.json") && b.Load(root/L"base.json"),"base load");
    Check(a.LoadParamsOverlay(root/L"game-a.params.json") && b.LoadParamsOverlay(root/L"game-b.params.json"),"params load");
    const auto first=ReadColorGradingSettings(a), second=ReadColorGradingSettings(b);
    Check(first.exposure.enabled && first.exposure.value==-1.25f && first.contrast.value==0,"values and zero persist");
    Check(first.lutEnabled && first.lutFile==filename && first.lutIntensity==.35f,"unicode punctuation LUT persists");
    Check(!second.exposure.enabled && second.saturation.enabled && second.saturation.value==1.75f && second.lutFile.empty(),"game isolation");
    Check(a.GetBool("masterEnabled",false) && a.GetBool("dlss5Enable",false) && !a.GetBool("nrProcessingEnabled",true),"master remains launcher owned");
    Put(root/L"bloom.params.json", R"JSON({"colorSharpenEnabled":true,"colorSharpen":3,"colorBloomEnabled":true,"colorBloom":3,"colorBloomThreshold":1.25,"colorBloomSoftKnee":0,"colorBloomRadius":2.5,"colorBloomScatter":0.9,"colorBloomSaturation":0.35})JSON");
    SettingsReader glow; Check(glow.Load(root/L"base.json") && glow.LoadParamsOverlay(root/L"bloom.params.json"),"bloom per-game overlay");
    const auto bloom=ReadColorGradingSettings(glow);
    Check(bloom.sharpen.enabled && bloom.sharpen.value==3 && bloom.bloom.enabled && bloom.bloom.value==3,"sharpen and bloom strength 3 survives profile reload");
    Check(bloom.bloomThreshold==1.25f && bloom.bloomSoftKnee==0 && bloom.bloomRadius==2.5f && bloom.bloomScatter==.9f && bloom.bloomSaturation==.35f,"all bloom detail values survive launcher-owned profile overlay");
    Check(second.bloomThreshold==.65f && second.bloomSoftKnee==.5f && second.bloomRadius==1 && second.bloomScatter==.7f && second.bloomSaturation==1 && !second.bloom.enabled,"old profiles use bloom defaults without enabling it or leaking another game's values");
    Check(!bloom.BasicOnly().AnyFinalActive() && bloom.FinalOnly().bloomRadius==2.5f,"bloom detail settings stay in final output stage");
    Put(root/L"bloom-bad.json", R"JSON({"colorSharpen":99,"colorBloom":99,"colorBloomThreshold":NaN,"colorBloomSoftKnee":2,"colorBloomRadius":-9,"colorBloomScatter":-1,"colorBloomSaturation":99})JSON");
    SettingsReader brokenBloom; Check(brokenBloom.Load(root/L"bloom-bad.json"),"invalid bloom inputs load");
    const auto sanitizedBloom=ReadColorGradingSettings(brokenBloom);
    Check(sanitizedBloom.sharpen.value==3 && sanitizedBloom.bloom.value==3 && sanitizedBloom.bloomThreshold==.65f && sanitizedBloom.bloomSoftKnee==1 && sanitizedBloom.bloomRadius==.25f && sanitizedBloom.bloomScatter==0 && sanitizedBloom.bloomSaturation==2 && !sanitizedBloom.AnyActive(),"advanced bloom bounds normalize without enabling a disabled effect");
    std::string invalid="{\"colorExposureEnabled\":true,\"colorExposure\":NaN,\"colorSaturation\":999,\"colorLutIntensity\":-2}";
    Put(root/L"bad.json",invalid); SettingsReader bad; Check(bad.Load(root/L"bad.json"),"invalid input load");
    const auto clean=ReadColorGradingSettings(bad);
    Check(clean.exposure.value==0 && clean.saturation.value==2 && clean.lutIntensity==0 && !clean.AnyActive(),"non-finite and bounds normalized");
    std::string decoded;
    Check(JsonScalar::Decode("\"\\u591c\\u666f,\\u007bLUT\\u007d.png\"",decoded) && decoded=="夜景,{LUT}.png","escaped unicode");
    Check(JsonScalar::Decode("\"\\ud83c\\udf08.png\"",decoded) && decoded=="🌈.png","surrogate pair");
    Check(!JsonScalar::Decode("\"\\ud800broken\"",decoded),"invalid surrogate rejected");
    const auto quoted=JsonScalar::Quote("a,}\\\"b.png");
    Check(JsonScalar::End(quoted+",\"next\":1",0)==quoted.size(),"quoted scalar boundary");
    Check(JsonScalar::Decode(quoted,decoded) && decoded=="a,}\\\"b.png","escaped filename roundtrip");
    const std::vector<PostFxSetting> effects{{"夜景, {电影}.fx",true,{.35f,-1.25f,0.0f}}, {"Second.fx",false,{2.0f}}};
    const auto encodedFx=SerializePostFxState(effects);
    const auto restoredFx=DeserializePostFxState(encodedFx);
    Check(restoredFx.size()==2 && restoredFx[0].file==effects[0].file && restoredFx[0].enabled &&
        restoredFx[0].values==effects[0].values && !restoredFx[1].enabled,"FX exact filename, enable and numeric roundtrip");
    Put(root/L"fx.params.json","{\"colorFxState\":"+JsonScalar::Quote(encodedFx)+"}");
    SettingsReader fxReader; Check(fxReader.Load(root/L"base.json") && fxReader.LoadParamsOverlay(root/L"fx.params.json"),"FX profile overlay");
    const auto fxSaved=ReadColorGradingSettings(fxReader);
    Check(fxSaved.fx.size()==2 && fxSaved.fx[0].values[0]==.35f && fxSaved.AnyFinalActive() &&
        !fxSaved.BasicOnly().AnyFinalActive() && ReadColorGradingSettings(b).fx.empty(),"FX per-game isolation and basic/final separation");
    for(const auto broken : {"[1,[\"A.fx\",true,[NaN]]]", "[1,[\"../A.fx\",true,[1]]]", "[1,[\"A.fx\",true,[1e999]]]",
        "[1,[\"A.fx\",true,[1,]]]", "[1,[\"A.fx\",true,[1]]]tail", "[2,[\"A.fx\",true,[1]]]",
        "[1,[\"A.fx\",true,[]],[\"a.FX\",false,[]]]", "[1,[\"A.dll\",true,[]]]"})
        Check(DeserializePostFxState(broken).empty(),"invalid FX settings fail closed");
    auto dirtyFx=effects; dirtyFx[0].values[0]=std::numeric_limits<float>::infinity();
    Check(DeserializePostFxState(SerializePostFxState(dirtyFx))[0].values[0]==0,"FX save never emits non-finite JSON");
    puts("PASS color settings: independent profiles, LUT Unicode/punctuation/escapes, zero and disabled values, neutral bypass, finite bounds, master ownership");
    return 0;
} catch (const std::exception& e) { fprintf(stderr,"FAIL %s\n",e.what());return 1; }
