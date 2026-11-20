#include "rv_game_config.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
std::string trim(std::string s) {
    auto notSpace=[](unsigned char c){return !std::isspace(c);};
    auto first=std::find_if(s.begin(),s.end(),notSpace);
    if(first==s.end()) return {};
    auto last=std::find_if(s.rbegin(),s.rend(),notSpace).base();
    return std::string(first,last);
}
std::string unquote(std::string s) {
    if(s.size()>=2 && ((s.front()=='"' && s.back()=='"') ||
                       (s.front()=='\'' && s.back()=='\'')))
        return s.substr(1,s.size()-2);
    return s;
}
}
bool LoadGameConfig(const std::string& filename, RVGameConfig& out) {
    namespace fs=std::filesystem;
    std::ifstream file(fs::u8path(filename));
    if(!file) {
        std::fprintf(stderr,"Cannot open game config: %s\n",filename.c_str());
        return false;
    }
    RVGameConfig loaded{};
    const fs::path base=fs::absolute(fs::u8path(filename)).parent_path();
    std::string line;
    unsigned lineNumber=0;
    while(std::getline(file,line)) {
        ++lineNumber;
        if(lineNumber==1 && line.size()>=3 &&
           (unsigned char)line[0]==0xef && (unsigned char)line[1]==0xbb &&
           (unsigned char)line[2]==0xbf) line.erase(0,3);
        line=trim(line);
        if(line.empty() || line[0]=='#' || line[0]==';') continue;
        const auto equals=line.find('=');
        if(equals==std::string::npos) {
            std::fprintf(stderr,"Config %s:%u: expected key=value\n",filename.c_str(),lineNumber);
            return false;
        }
        const std::string key=trim(line.substr(0,equals));
        const std::string value=unquote(trim(line.substr(equals+1)));
        std::string* target=nullptr;
        if(key=="car") target=&loaded.car;
        else if(key=="script") target=&loaded.script;
        else if(key=="pavement") target=&loaded.pavement;
        else if(key=="character") target=&loaded.character;
        else if(key=="idle") target=&loaded.idle;
        else if(key=="walking") target=&loaded.walking;
        else if(key=="jump") target=&loaded.jump;
        else if(key=="enter_car") target=&loaded.enterCar;
        else if(key.rfind("building_slot_",0)==0) {
            const std::string suffix=key.substr(14);
            const bool digits=!suffix.empty() && std::all_of(suffix.begin(),suffix.end(),[](unsigned char c){return std::isdigit(c)!=0;});
            if(digits) {
                const unsigned long slot=std::stoul(suffix);
                if(slot>=1 && slot<=loaded.buildingSlots.size()) target=&loaded.buildingSlots[slot-1];
            }
        }
        else {
            std::fprintf(stderr,"Config %s:%u: unknown key '%s'\n",
                         filename.c_str(),lineNumber,key.c_str());
            return false;
        }
        if(value.empty()) { target->clear();continue; }
        const fs::path path=fs::u8path(value);
        *target=(path.is_absolute()?path:base/path).lexically_normal().u8string();
    }
    if(loaded.script.empty()) {
        std::fprintf(stderr,"Config requires script=...\n");
        return false;
    }
    out=std::move(loaded);
    std::fprintf(stderr,"Game config loaded: %s\n",filename.c_str());
    return true;
}
