    bool growthOpen_=false;
    MenuButtonPos GrowthButton(int i)const{float w=SaveUiWidth(),h=SaveUiHeight(),m=std::min(w,h);if(i==0)return {w*.5f-m*.56f,h*.145f,m*.15f,m*.033f,m*.012f};return {w*.5f+m*.25f,h*(.36f+.14f*(i-1)),m*.12f,m*.042f,m*.012f};}
    bool GrowthTap(float x,float y){
        if(growthOpen_){if(HitRect(x,y,GrowthButton(0)))growthOpen_=false;else for(int i=1;i<=3;++i)if(HitRect(x,y,GrowthButton(i)))inventory_.growth.Spend(i-1);return true;}
        if(HitRect(x,y,GrowthButton(0))){growthOpen_=true;return true;}return false;
    }
    bool DrawGrowthUi(){float w=SaveUiWidth(),h=SaveUiHeight(),m=std::min(w,h);const float white[4]={1,1,1,1},panel[4]={.08f,.13f,.19f,.98f},accent[4]={.16f,.33f,.45f,.96f};
        auto b=GrowthButton(0);
        if(!growthOpen_){DrawMenuRect(b,accent);DrawTextCentered("角色加点",b.cx,b.cy,m*.025f,white);return false;}
        const float backdrop[4]={.02f,.035f,.055f,.6f};DrawMenuRect({w*.5f,h*.5f,w*.5f,h*.5f,0},backdrop);DrawMenuRect({w*.5f,h*.51f,std::min(w*.88f,m*1.65f)*.5f,h*.42f,m*.02f},panel);DrawTextCentered("角色加点",w*.5f,h*.145f,m*.043f,white);DrawMenuRect(b,accent);DrawTextCentered("返回背包",b.cx,b.cy,m*.025f,white);
        char text[128];const auto& g=inventory_.growth;std::snprintf(text,sizeof(text),"等级 %d    可用点数 %d",g.level,g.points);DrawTextCentered(text,w*.5f,h*.22f,m*.04f,white);
        const char* names[]={"力量  攻击","防御  减伤","敏捷  移速"};const int values[]={g.strength,g.defense,g.agility};
        for(int i=0;i<3;++i){auto plus=GrowthButton(i+1);std::snprintf(text,sizeof(text),"%s  %d  (+%d%%)",names[i],values[i],values[i]*(i==2?2:5));DrawTextCentered(text,w*.5f-m*.18f,plus.cy,m*.028f,white);DrawMenuRect(plus,accent);DrawTextCentered(g.points?"+1":"无点数",plus.cx,plus.cy,m*.025f,white);}
        DrawTextCentered("每次升级获得3点 分配后在存档点保存",w*.5f,h*.86f,m*.025f,white);return true;
    }
// Shared renderer-owned save flow and menu. Included inside each renderer class.
    bool saveDeleteMode_=false;
    unsigned saveDeleteId_=0;
    void EnsureSaveStore() {
        if (saveStoreReady_) return;
        const char* root=OHOS_GetSaveDirectory();
        if(root && *root) { saveRoot_=std::string(root)+"/"; saveStore_.Open(saveRoot_); }
        else SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"SDL3_SAVE stage=storage_unavailable");
        saveStoreReady_=true;
    }
    void SaveMessage(const char* text) { saveMessage_=text;saveMessageUntil_=SDL_GetTicks()+3500; }
    const scene::Vec3* NearSavePoint() const {
        if(!sceneDefinition_ || uiScreen_!=rhi::UiScreen::Gameplay || playerHealth_<=0) return nullptr;
        for(const auto& p:sceneDefinition_->savePoints) {
            float dx=p.x-playerX_,dz=p.z-playerZ_;
            if(dx*dx+dz*dz<=2.25f && std::fabs(p.y-playerY_)<.45f &&
               (!physicsWorld_ || physicsWorld_->GetPlayerState().grounded)) return &p;
        }
        return nullptr;
    }
    saves::Record CaptureSave(const scene::Vec3& point) {
        saves::Record r;r.id=activeSaveId_;r.savedAt=std::time(nullptr);r.scene=activeSceneKey_;
        r.x=point.x;r.y=point.y;r.z=point.z;r.yaw=playerYaw_;r.health=playerHealth_;
        r.gold=gold_;r.inventory=inventory_;r.ammo=pistol_.ammo;r.reserve=pistol_.reserve;
        r.weapon=pistol_.equipped?0:(sword_.drawn?1:-1);
        r.parkourRewardClaimed=parkourProgress_.rewardClaimed;
        r.courseRevision=sceneDefinition_->parkour.enabled?sceneDefinition_->parkour.revision:0;
        r.checkpoint=int(parkourProgress_.checkpoint);r.falls=parkourProgress_.falls;r.seconds=parkourProgress_.seconds;
        r.gateOpen=physicsWorld_ && physicsWorld_->GetWorkshopState().gateOpen;
        r.chestOpened=ChestOpened();r.enemyDead=sceneDefinition_->enemyEnabled && !enemyAlive_;
        return r;
    }
    void SaveAtPoint() {
        const auto* p=NearSavePoint();if(!p || !activeSaveId_)return;
        auto record=CaptureSave(*p);
        if(saveStore_.Put(record)) {SaveMessage("存档成功");SDL_Log("SDL3_SAVE stage=saved id=%u scene=%s",record.id,record.scene.c_str());}
        else SaveMessage("存档失败请重试");
    }
    void OpenSaveList() { EnsureSaveStore();if(!saveRoot_.empty())saveStore_.Open(saveRoot_);savePage_=0;saveDeleteMode_=false;saveDeleteId_=0;saveSelection_=true; }
    void BeginSaveLoad(const saves::Record& record) {
        pendingSave_=record;pendingSaveLoad_=true;saveSelection_=false;
        if(record.scene==activeSceneKey_) { ResetGameplayScene();ApplySave(); }
        else {saveSceneRequest_=record.scene;uiScreen_=rhi::UiScreen::MainMenu;}
    }
    void ApplySave() {
        const auto r=pendingSave_;pendingSaveLoad_=false;activeSaveId_=r.id;
        playerX_=r.x;playerY_=r.y;playerZ_=r.z;playerYaw_=r.yaw;playerHealth_=r.health;
        gold_=r.gold;inventory_=r.inventory;pistol_.ammo=r.ammo;pistol_.reserve=r.reserve;
        pistol_.equipped=r.weapon==0 && inventory_.HasPistol();sword_.drawn=r.weapon==1 && inventory_.HasSword();
        parkourProgress_.checkpoint=sceneDefinition_->parkour.checkpoints.empty()?0:
            std::min(std::size_t(r.checkpoint),sceneDefinition_->parkour.checkpoints.size()-1);
        parkourProgress_.falls=r.falls;parkourProgress_.seconds=r.seconds;
        parkourProgress_.finished=sceneDefinition_->parkour.enabled && parkourProgress_.checkpoint+1==sceneDefinition_->parkour.checkpoints.size();
        parkourProgress_.rewardClaimed=r.parkourRewardClaimed || parkourProgress_.finished;
        const bool revisedCourse=sceneDefinition_->parkour.enabled && r.courseRevision!=sceneDefinition_->parkour.revision;
        if(revisedCourse) {
            parkourProgress_={};
            const auto& spawn=sceneDefinition_->playerSpawn;
            playerX_=spawn.x;playerY_=spawn.y;playerZ_=spawn.z;
            SaveMessage("路线已更新 从起点开始");
        }
        chestOpenStartTicks_=r.chestOpened?SDL_GetTicks():0;
        if(r.enemyDead) {enemyAlive_=enemyVisible_=false;enemyHealth_=0;}
        if(physicsWorld_ && physicsWorld_->IsReady()) {
            physicsWorld_->RestoreGateProgress(r.gateOpen && !revisedCourse);
            physicsWorld_->RespawnPlayer(playerX_,playerY_-physics::kPlayerVisualOriginOffset,playerZ_);
            physicsWorld_->SetEnemyCollisionEnabled(enemyAlive_);
        }
        camPanX_=playerX_;camPanY_=playerY_+camera::kHeadFocusOffset;camPanZ_=playerZ_;
        cameraObstruction_.Reset();portalReadyTicks_=SDL_GetTicks()+2500;
        uiScreen_=rhi::UiScreen::Gameplay;cameraInput_={};
        SDL_Log("SDL3_SAVE stage=loaded id=%u scene=%s",r.id,r.scene.c_str());
    }
    MenuButtonPos SaveButton(int i) const {
        float w=SaveUiWidth(),h=SaveUiHeight(),m=std::min(w,h);
        if(i==7)return {w-m*.12f,m*.09f,m*.08f,m*.035f,m*.014f};
        if(i<3) return {w*.5f,h*(.31f+.16f*i),m*.48f,m*.065f,m*.016f};
        if(i==3)return {w*.5f,h*.83f,m*.24f,m*.052f,m*.024f};
        if(i==4)return {m*.12f,m*.09f,m*.08f,m*.035f,m*.014f};
        return {w*.5f+(i==5?-1:1)*m*.35f,h*.83f,m*.07f,m*.042f,m*.014f};
    }
    void HandleSaveTap(float x,float y) {
        if(pendingSaveLoad_)return;
        if(saveDeleteId_) {
            if(HitRect(x,y,SaveButton(4))) {saveDeleteId_=0;return;}
            if(HitRect(x,y,SaveButton(3))) {
                const unsigned id=saveDeleteId_;
                if(saveStore_.Remove(id)) {
                    if(activeSaveId_==id)activeSaveId_=0;
                    saveDeleteId_=0;
                    savePage_=std::min(savePage_,std::max(0,(int(saveStore_.records.size())-1)/3));
                    if(saveStore_.records.empty())saveDeleteMode_=false;
                    SaveMessage("存档已删除");
                } else SaveMessage("删除失败请重试");
            }
            return;
        }
        if(HitRect(x,y,SaveButton(7))) {if(!saveStore_.records.empty())saveDeleteMode_=!saveDeleteMode_;return;}
        if(HitRect(x,y,SaveButton(4))) {saveSelection_=false;return;}
        if(HitRect(x,y,SaveButton(5))) {if(savePage_>0)--savePage_;return;}
        if(HitRect(x,y,SaveButton(6))) {if((savePage_+1)*3<int(saveStore_.records.size()))++savePage_;return;}
        if(HitRect(x,y,SaveButton(3)) && !saveDeleteMode_) {
            saves::Record fresh;fresh.id=saveStore_.NextId();fresh.savedAt=std::time(nullptr);
            // The hub's render origin is the original gameplay spawn.
            fresh.x=0;fresh.y=.02f;fresh.z=3;
            if(!saveStore_.Put(fresh)) {SaveMessage("存档失败请重试");return;}
            BeginSaveLoad(fresh);return;
        }
        for(int i=0;i<3;++i) {int at=savePage_*3+i;if(at<int(saveStore_.records.size())&&HitRect(x,y,SaveButton(i))){if(saveDeleteMode_)saveDeleteId_=saveStore_.records[at].id;else BeginSaveLoad(saveStore_.records[at]);return;}}
    }
    void DrawSaveList() {
        float w=SaveUiWidth(),h=SaveUiHeight(),m=std::min(w,h);
        const float white[4]={1,1,1,.96f},panel[4]={.07f,.13f,.21f,.85f},accent[4]={.12f,.42f,.55f,.95f};
        const float danger[4]={.65f,.16f,.18f,.95f};
        if(saveDeleteId_) {
            DrawTextCentered("删除存档",w*.5f,h*.12f,m*.054f,white);
            const std::string prompt="确认删除存档 "+std::to_string(saveDeleteId_)+"？";
            DrawTextCentered(prompt.c_str(),w*.5f,h*.40f,m*.038f,white);
            DrawTextCentered("删除后无法恢复",w*.5f,h*.50f,m*.028f,white);
            auto confirm=SaveButton(3);DrawMenuRect(confirm,danger);
            DrawTextCentered("确认删除",confirm.cx,confirm.cy,m*.027f,white);
            auto cancel=SaveButton(4);DrawMenuRect(cancel,panel);
            DrawTextCentered("取消",cancel.cx,cancel.cy,m*.027f,white);
            if(SDL_GetTicks()<saveMessageUntil_)DrawTextCentered(saveMessage_.c_str(),w*.5f,h*.72f,m*.025f,white);
            return;
        }
        auto remove=SaveButton(7);DrawMenuRect(remove,saveDeleteMode_?danger:panel);
        DrawTextCentered(saveDeleteMode_?"完成":"删除",remove.cx,remove.cy,m*.027f,white);
        if(saveDeleteMode_)DrawTextCentered("点击要删除的存档",w*.5f,h*.20f,m*.027f,white);
        DrawTextCentered("选择存档",w*.5f,h*.12f,m*.054f,white);
        if(saveStore_.records.empty())DrawTextCentered("没有存档请创建新游戏",w*.5f,h*.43f,m*.035f,white);
        for(int i=0;i<3;++i) {int at=savePage_*3+i;if(at>=int(saveStore_.records.size()))break;
            const auto& r=saveStore_.records[at];auto b=SaveButton(i);DrawMenuRect(b,panel);
            const char* place=r.scene=="scenes/main.json"?"主场景":(r.scene=="scenes/forest_test.json"?"森林场景":"跑酷场景");
            std::string title="存档 "+std::to_string(r.id)+"  "+place;
            DrawTextCentered(title.c_str(),b.cx,b.cy-b.hh*.4f,m*.029f,white);
            std::string date="最后保存 "+saves::Date(r.savedAt);
            DrawTextCentered(date.c_str(),b.cx,b.cy+b.hh*.45f,m*.024f,white);
        }
        for(int i=3;i<=6;++i) {
            if(i==3 && saveDeleteMode_)continue;
            if((i==5&&savePage_==0)||(i==6&&(savePage_+1)*3>=int(saveStore_.records.size())))continue;
            auto b=SaveButton(i);DrawMenuRect(b,i==3?accent:panel);
            DrawTextCentered(i==3?"创建新游戏":(i==4?"返回":(i==5?"上页":"下页")),b.cx,b.cy,m*.027f,white);
        }
        std::string page=std::to_string(savePage_+1)+" / "+std::to_string(std::max(1,(int(saveStore_.records.size())+2)/3));
        DrawTextCentered(page.c_str(),w*.5f,h*.94f,m*.023f,white);
        if(SDL_GetTicks()<saveMessageUntil_)DrawTextCentered(saveMessage_.c_str(),w*.5f,h*.72f,m*.025f,white);
    }
