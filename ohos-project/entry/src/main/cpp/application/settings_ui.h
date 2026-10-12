#pragma once
#include <algorithm>
#include <cmath>
namespace settings_ui {
struct Button {float cx,cy,hw,hh,r;};
inline float MaxScroll(float w,float h){float mn=std::min(w,h);return std::max(0.f,(7*.105f+.08f)*mn-(h*.78f-h*.28f));}
inline Button Layout(int i,float w,float h,float scroll){float mn=std::min(w,h);
    if(i==0)return {w*.5f+mn*.39f,h*.19f,mn*.027f,mn*.027f,mn*.012f};
    if(i==8)return {w*.5f,h*.86f,mn*.30f,mn*.040f,mn*.014f};
    return {w*.5f+mn*.23f,h*.28f+mn*.045f+(i-1)*mn*.105f-scroll,mn*.13f,mn*.035f,mn*.009f};
}
inline bool Visible(int i,float w,float h,float scroll){auto b=Layout(i,w,h,scroll);return i==0 || i==8 || (b.cy-b.hh>=h*.28f && b.cy+b.hh<=h*.78f);}
template<class Rect,class Text>
void Draw(float w,float h,float scroll,const char values[7][32],bool glass,Rect rect,Text text){
    float mn=std::min(w,h),left=w*.5f-mn*.36f;
    const float bg[4]={.015f,.025f,.045f,.38f},rim[4]={.68f,.82f,.94f,.82f};
    const float panel[4]={.10f,.16f,.23f,.97f},row[4]={.12f,.20f,.28f,.82f};
    const float control[4]={.16f,.33f,.45f,.96f},white[4]={1,1,1,1};
    rect(w*.5f,h*.5f,w*.5f,h*.5f,0,bg);
    rect(w*.5f,h*.5f,mn*.445f,h*.39f,mn*.024f,rim);
    rect(w*.5f,h*.5f,mn*.442f,h*.39f-mn*.003f,mn*.022f,panel);
    text("设置",left,h*.19f,mn*.045f,white,false);
    auto close=Layout(0,w,h,scroll);rect(close.cx,close.cy,close.hw,close.hh,close.r,control);text("X",close.cx,close.cy,mn*.025f,white,true);
    static const char* labels[7]={"调试面板","视角灵敏度","泛光","渲染分辨率","UI透明度","阴影质量","液态玻璃"};
    for(int i=1;i<=7;i++)if(Visible(i,w,h,scroll)){
        auto b=Layout(i,w,h,scroll);rect(w*.5f,b.cy,mn*.405f,mn*.044f,mn*.010f,row);
        rect(b.cx,b.cy,b.hw,b.hh,b.r,control);
        text(labels[i-1],left,b.cy,mn*.028f,white,false);text(values[i-1],b.cx,b.cy,mn*.027f,white,true);
    }
    auto back=Layout(8,w,h,scroll);rect(back.cx,back.cy,back.hw,back.hh,back.r,control);text("返回主菜单",back.cx,back.cy,mn*.028f,white,true);
    float max=MaxScroll(w,h);
    if(max>0){float top=h*.28f,bottom=h*.78f,thumb=(bottom-top)*(bottom-top)/(bottom-top+max);
        rect(w*.5f+mn*.424f,(top+bottom)*.5f,mn*.0015f,(bottom-top)*.5f,0,row);
        rect(w*.5f+mn*.424f,top+thumb*.5f+(bottom-top-thumb)*scroll/max,mn*.002f,thumb*.5f,mn*.002f,white);}
}
}
