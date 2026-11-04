#pragma once
namespace helmet_display {
constexpr int kCount=4;
constexpr float kX=-8.0f;
inline float Z(int i){return 4.0f-3.0f*i;}
inline int Mode(int i){return i<3?i+1:0;}
inline const char* Label(int i){static const char* labels[]={"ALBEDO","WORLD NORMAL","PBR: R=METAL G=ROUGH B=AO","FULL PBR LIGHTING"};return labels[i];}
}
