#pragma once
namespace progression {
struct State {
 int level=1,xp=0,points=0,strength=0,defense=0,agility=0;
 int Required()const{return 60+(level-1)*40;}
 void Gain(int amount){if(amount<=0||level>=50)return;xp+=amount;while(level<50&&xp>=Required()){xp-=Required();++level;points+=3;}if(level==50)xp=0;}
 bool Spend(int stat){if(points<=0||stat<0||stat>2)return false;--points;if(stat==0)++strength;else if(stat==1)++defense;else ++agility;return true;}
 bool Valid()const{return level>=1&&level<=50&&xp>=0&&xp<Required()&&(level<50||xp==0)&&points>=0&&strength>=0&&defense>=0&&agility>=0&&points<=147&&strength<=147&&defense<=147&&agility<=147&&points+strength+defense+agility==(level-1)*3;}
};
}
