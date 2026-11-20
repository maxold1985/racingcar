#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// Minimal ESRI Shapefile reader for projected polyline road layers.
// SHP XY must be in METERS (projected CRS). Geographic lat/lon is rejected.
// Z/M values are ignored; roads are rendered on the existing Y=0 ground.
namespace rv_shp {
struct Segment { float x1,z1,x2,z2,width; };
struct Result { std::vector<Segment> segments; double originX=0,originY=0; std::string error; };
inline std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
}
inline std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
inline double leDouble(const unsigned char* p) {
    std::uint64_t bits=0;
    for(int i=0;i<8;++i) bits|=std::uint64_t(p[i])<<(8*i);
    double v; std::memcpy(&v,&bits,8); return v;
}
inline Result readRoads(const std::string& path,float width=8.0f,
                        double originX=std::numeric_limits<double>::quiet_NaN(),
                        double originY=std::numeric_limits<double>::quiet_NaN()) {
    Result result;
    std::ifstream in(path,std::ios::binary);
    if(!in) { result.error="Cannot open SHP: "+path; return result; }
    unsigned char header[100]{};
    if(!in.read(reinterpret_cast<char*>(header),100) || be32(header)!=9994 ||
       le32(header+28)!=1000) {
        result.error="Invalid SHP header"; return result;
    }
    const std::uint32_t type=le32(header+32);
    if(type!=3 && type!=13 && type!=23) {
        result.error="Expected PolyLine SHP (types 3, 13 or 23)"; return result;
    }
    if(!std::isfinite(width)||width<=0) { result.error="Invalid road width";return result; }
    // Typical unprojected WGS84 coordinates are degrees, not meters.
    const double minX=leDouble(header+36),minY=leDouble(header+44);
    const double maxX=leDouble(header+52),maxY=leDouble(header+60);
    if(minX>=-180 && maxX<=180 && minY>=-90 && maxY<=90) {
        result.error="SHP appears to contain degrees; reproject to a metric CRS in QGIS";
        return result;
    }
    const bool autoOrigin=!std::isfinite(originX)||!std::isfinite(originY);
    result.originX=autoOrigin?minX:originX;
    result.originY=autoOrigin?minY:originY;
    const auto readBlock=[&](std::vector<unsigned char>& b,std::size_t n)->bool {
        b.resize(n);return static_cast<bool>(in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(n)));
    };
    std::vector<unsigned char> record;
    while(true) {
        unsigned char rh[8]{};
        if(!in.read(reinterpret_cast<char*>(rh),8)) {
            if(in.eof() && in.gcount()==0) break;
            result.error="Truncated SHP record header"; result.segments.clear();return result;
        }
        const std::uint64_t bytes=std::uint64_t(be32(rh+4))*2;
        if(bytes<4 || bytes>128ull*1024*1024 || !readBlock(record,static_cast<std::size_t>(bytes))) {
            result.error="Invalid/truncated SHP record";result.segments.clear();return result;
        }
        const auto* p=record.data();
        const std::uint32_t shape=le32(p);
        if(shape==0) continue;
        if(shape!=3 && shape!=13 && shape!=23) continue;
        if(bytes<44) {result.error="Truncated polyline";result.segments.clear();return result;}
        const std::uint32_t parts=le32(p+36),points=le32(p+40);
        if(parts==0 || points<2 || parts>points ||
           std::uint64_t(44)+std::uint64_t(parts)*4+std::uint64_t(points)*16>bytes) {
            result.error="Invalid polyline parts/points";result.segments.clear();return result;
        }
        const std::size_t pointStart=44+std::size_t(parts)*4;
        for(std::uint32_t part=0;part<parts;++part) {
            const std::uint32_t begin=le32(p+44+part*4);
            const std::uint32_t end=part+1<parts?le32(p+44+(part+1)*4):points;
            if(begin>=end || end>points) {
                result.error="Invalid part offset";result.segments.clear();return result;
            }
            for(std::uint32_t j=begin+1;j<end;++j) {
                const auto* a=p+pointStart+std::size_t(j-1)*16;
                const auto* b=p+pointStart+std::size_t(j)*16;
                const double ax=leDouble(a)-result.originX,az=leDouble(a+8)-result.originY;
                const double bx=leDouble(b)-result.originX,bz=leDouble(b+8)-result.originY;
                if(!std::isfinite(ax)||!std::isfinite(az)||!std::isfinite(bx)||!std::isfinite(bz)||
                   std::abs(ax)>1e7||std::abs(az)>1e7||std::abs(bx)>1e7||std::abs(bz)>1e7) continue;
                if(std::hypot(bx-ax,bz-az)<0.01) continue;
                result.segments.push_back({float(ax),float(az),float(bx),float(bz),width});
            }
        }
    }
    return result;
}
}
