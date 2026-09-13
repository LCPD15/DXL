#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace DXL {
struct PostFxParameter {
    std::string key, titleEn, titleZh;
    float minimum=0, maximum=1, defaultValue=0;
};
struct PostFxDefinition {
    std::string file, titleEn, titleZh;
    std::vector<PostFxParameter> parameters;
    std::string error;
    uint64_t fingerprint=0;
    std::string source; // Validated, bounded source; never follows include files.
    bool reshade = false;
};
// Identify the DXL entry point without mistaking comments or quoted include
// paths for code. Everything else is compiled by the native ReShade parser.
inline bool HasDxlFxEntry(const std::string& source) {
    for (size_t i=0;i<source.size();) {
        if (source[i]=='/' && i+1<source.size() && source[i+1]=='/') {
            i=source.find('\n',i+2); if(i==std::string::npos)break;
        } else if (source[i]=='/' && i+1<source.size() && source[i+1]=='*') {
            i=source.find("*/",i+2); if(i==std::string::npos)break; i+=2;
        } else if (source[i]=='\"' || source[i]=='\'') {
            const char quote=source[i++];
            while(i<source.size()) { if(source[i]=='\\')i+=2; else if(source[i++]==quote)break; }
        } else if (std::isalpha(static_cast<unsigned char>(source[i])) || source[i]=='_') {
            const auto start=i++;
            while(i<source.size()&&(std::isalnum(static_cast<unsigned char>(source[i]))||source[i]=='_'))++i;
            if(source.compare(start,i-start,"DXL_Effect")==0)return true;
        } else ++i;
    }
    return false;
}
inline std::wstring PostFxWide(const std::string& value) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(count<=0)return {};
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count);
    return result;
}
inline std::string PostFxUtf8(const std::wstring& value) {
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(count<=0)return {};
    std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr);
    return result;
}
inline bool ValidatePostFxFilename(const std::string& name) noexcept {
    if(name.size()<4||name.size()>240||name.front()=='.'||name.back()=='.'||name.back()==' ')return false;
    for(unsigned char c:name)if(c<32||c==127||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;
    const auto n=name.size();
    return name[n-3]=='.'&&(name[n-2]=='f'||name[n-2]=='F')&&(name[n-1]=='x'||name[n-1]=='X')&&
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0)>0;
}
inline bool PostFxFilenameLess(const std::string& a,const std::string& b) {
    const auto wa=PostFxWide(a),wb=PostFxWide(b);
    const int comparison=CompareStringOrdinal(wa.data(),static_cast<int>(wa.size()),wb.data(),static_cast<int>(wb.size()),TRUE);
    return comparison==CSTR_LESS_THAN||(comparison==CSTR_EQUAL&&a<b);
}
inline bool PostFxFilenameEqual(const std::string& a,const std::string& b) {
    const auto wa=PostFxWide(a),wb=PostFxWide(b);
    return !wa.empty() && !wb.empty() && CompareStringOrdinal(wa.data(),static_cast<int>(wa.size()),wb.data(),static_cast<int>(wb.size()),TRUE)==CSTR_EQUAL;
}
inline std::filesystem::path PostFxRoot(HMODULE module) {
    std::wstring file(32768,L'\0');
    const auto n=GetModuleFileNameW(module,file.data(),static_cast<DWORD>(file.size()));
    if(!n||n>=file.size())return {};
    file.resize(n);
    return std::filesystem::path(file).parent_path()/L"post-processing";
}
inline std::string PostFxTrim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");
    return first==std::string::npos?std::string{}:value.substr(first,last-first+1);
}
inline PostFxDefinition ReadPostFxDefinition(const std::filesystem::path& root,const std::string& file) {
    PostFxDefinition result;result.file=file;result.titleEn=result.titleZh=file;
    if(!ValidatePostFxFilename(file)||root.empty()){result.error="Invalid .fx filename";return result;}
    const DWORD folder=GetFileAttributesW(root.c_str());
    if(folder==INVALID_FILE_ATTRIBUTES||(folder&FILE_ATTRIBUTE_REPARSE_POINT)){result.error="Post-processing folder is missing or is a link";return result;}
    const auto path=root/PostFxWide(file);
    WIN32_FILE_ATTRIBUTE_DATA stat{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&stat)||(stat.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
        stat.nFileSizeHigh||stat.nFileSizeLow>256*1024){result.error="Effect is missing, is a link, or exceeds 256 KiB";return result;}
    result.fingerprint=(uint64_t(stat.ftLastWriteTime.dwHighDateTime)<<32)|stat.ftLastWriteTime.dwLowDateTime;
    result.fingerprint^=uint64_t(stat.nFileSizeLow)*1099511628211ull;
    std::ifstream input(path,std::ios::binary);
    if(!input){result.error="Cannot read effect file";return result;}
    result.source.resize(stat.nFileSizeLow);input.read(result.source.data(),result.source.size());
    if(!input||result.source.find('\0')!=std::string::npos){result.error="Cannot read effect source text";return result;}
    if(result.source.starts_with("\xef\xbb\xbf"))result.source.erase(0,3);
    result.reshade=!HasDxlFxEntry(result.source);
    if(result.reshade)return result;
    std::unordered_set<std::string> keys;
    std::istringstream lines(result.source);std::string line;
    while(std::getline(lines,line)) {
        const auto text=PostFxTrim(line);
        if(!text.starts_with("// @dxl "))continue;
        const auto directive=text.substr(8);
        if(directive.starts_with("name_en "))result.titleEn=PostFxTrim(directive.substr(8));
        else if(directive.starts_with("name_zh "))result.titleZh=PostFxTrim(directive.substr(8));
        else if(directive.starts_with("param ")) {
            auto fields=directive.substr(6);const auto bar=fields.find('|');
            std::istringstream values(fields.substr(0,bar));PostFxParameter p;
            if(!(values>>p.key>>p.minimum>>p.maximum>>p.defaultValue)||p.key.empty()||p.key.size()>48||
                !std::all_of(p.key.begin(),p.key.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';})||
                !std::isfinite(p.minimum)||!std::isfinite(p.maximum)||!std::isfinite(p.defaultValue)||
                p.minimum>p.maximum||p.minimum< -10000||p.maximum>10000||p.defaultValue<p.minimum||p.defaultValue>p.maximum||
                !keys.insert(p.key).second||result.parameters.size()>=16){result.error="Invalid or duplicate parameter metadata (maximum 16)";return result;}
            std::string excess;if(values>>excess){result.error="Unexpected parameter metadata";return result;}
            p.titleEn=p.titleZh=p.key;
            if(bar!=std::string::npos){auto names=fields.substr(bar+1);const auto split=names.find('|');p.titleEn=PostFxTrim(names.substr(0,split));p.titleZh=split==std::string::npos?p.titleEn:PostFxTrim(names.substr(split+1));}
            result.parameters.push_back(std::move(p));
        } else {result.error="Unknown @dxl metadata directive";return result;}
    }
    return result;
}
inline std::vector<PostFxDefinition> EnumeratePostFx(HMODULE module, bool includeReShade=false) {
    std::vector<PostFxDefinition> result;
    try {
        const auto root=PostFxRoot(module);
        if(root.empty()||!std::filesystem::is_directory(root))return result;
        const DWORD attributes=GetFileAttributesW(root.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return result;
        std::vector<std::string> names;
        for(const auto& file:std::filesystem::directory_iterator(root)) {
            const auto name=PostFxUtf8(file.path().filename().wstring());
            if(ValidatePostFxFilename(name))names.push_back(name);
        }
        std::sort(names.begin(),names.end(),PostFxFilenameLess);
        if(names.size()>64)names.resize(64);
        for(const auto& name:names) {
            auto definition=ReadPostFxDefinition(root,name);
            if(includeReShade || !definition.reshade)result.push_back(std::move(definition));
        }
    }catch(...){}
    return result;
}
}
