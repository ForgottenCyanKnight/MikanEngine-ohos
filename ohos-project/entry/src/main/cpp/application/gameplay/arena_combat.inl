// Shared wave combat; native renderers supply animation/state adapters.
    void UpdateArena(float dt,int pressed) {
        const auto tick=[dt](float& timer){timer=std::max(0.f,timer-dt);};
        tick(playerAttackCooldown_);tick(playerHitLockTimer_);tick(playerHitFlashTimer_);
        tick(playerAttackInvulnerability_);tick(playerHitInvulnerability_);
        playerAttackAcceptedThisFrame_=false;
        if(!playerSwimming_ && !pistol_.equipped && (pressed&1) && playerAttackCooldown_<=0 && playerHitLockTimer_<=0) {
            playerAttackCooldown_=std::max(kMinimumPlayerAttackCooldown,ArenaAttackDuration());
            playerAttackInvulnerability_=kPlayerAttackInvulnerability;playerAttackAcceptedThisFrame_=true;
        }
        for(const auto& clip:ArenaPlayerClips())if(clip.name=="Pistol_Reload" && clip.duration>0)pistol_.reloadDuration=clip.duration;
        if(pistol_.equipped && (pressed&1))pistol_.pendingFire=true;
        pistol_.Tick(dt,!playerSwimming_ && playerHitLockTimer_<=0);
        int shotTarget=-1;float closest=60;
        if(pistol_.fired) {
            AudioManager::GetInstance().PlayAudio("pistol",.65f);
            for(int axis=0;axis<3;++axis)shotMuzzle_[axis]=pistolMatrix_.value[12+axis]+pistolMatrix_.value[axis]*.95f+pistolMatrix_.value[4+axis]*.35f;
            float dx=std::sin(camYaw_)*std::cos(camPitch_),dy=-std::sin(camPitch_),dz=-std::cos(camYaw_)*std::cos(camPitch_);
            if(physicsWorld_)closest=physicsWorld_->RaycastWorld(camPosX_,camPosY_,camPosZ_,dx,dy,dz,60);
            for(int i=0;i<4;++i)if(arenaEnemies_[i].Alive) {
                const auto& a=arenaEnemies_[i];float hit=gameplay::RayEnemy(camPosX_,camPosY_,camPosZ_,dx,dy,dz,a.X,a.Y,a.Z);
                if(hit<closest) {closest=hit;shotTarget=i;}
            }
            if(shotTarget>=0 && physicsWorld_ && pistolPoseReady_) {
                float mx=pistolMatrix_.value[12],my=pistolMatrix_.value[13],mz=pistolMatrix_.value[14];
                float tx=camPosX_+dx*closest-mx,ty=camPosY_+dy*closest-my,tz=camPosZ_+dz*closest-mz;
                float distance=std::sqrt(tx*tx+ty*ty+tz*tz);
                if(distance>.001f && physicsWorld_->RaycastWorld(mx,my,mz,tx/distance,ty/distance,tz/distance,distance)<distance-.01f)shotTarget=-1;
            }
            pistol_.hit=shotTarget>=0;if(pistol_.hit)pistol_.hitFlash=.18f;
        }
        for(int i=0;i<4;++i) {
            auto& actor=arenaEnemies_[i];if(!actor.Alive)continue;
            SwapArena(actor);
            tick(enemyAttackCooldown_);tick(enemyHitInvulnerability_);tick(enemyHitLockTimer_);
            float dx=playerX_-enemyX_,dz=playerZ_-enemyZ_,distance=std::sqrt(dx*dx+dz*dz);
            bool melee=playerAttackAcceptedThisFrame_ && distance<=(sword_.drawn?2.4f:1.75f) &&
                std::fabs(playerY_-enemyY_)<=1.5f && enemyHitInvulnerability_<=0;
            if(melee && physicsWorld_ && distance>.01f)
                melee=physicsWorld_->RaycastWorld(playerX_,playerY_,playerZ_,dx/distance,0,dz/distance,distance)>=distance-.01f;
            if(melee || shotTarget==i) {
                enemyHealth_=std::max(0.f,enemyHealth_-25*inventory_.Stats(shotTarget==i?0:(sword_.drawn?5:inventory::kNoItem)).attack);
                enemyHitInvulnerability_=kHitInvulnerability;enemyHitLockTimer_=kPlayerHitDuration;
                AudioManager::GetInstance().PlayAudio("hit",.55f);
                if(enemyHealth_<=0) {
                    enemyAlive_=false;enemyDeathPlaying_=true;enemyDeathTime_=0;
                    enemyMoving_=enemyAttackRequested_=false;gold_+=25;inventory_.growth.Gain(50);
                    if(physicsWorld_)physicsWorld_->SetArenaEnemy(i,enemyX_,enemyY_-physics::kPlayerVisualOriginOffset,enemyZ_,false);
                }
            }
            enemyMoving_=false;
            float vx=0,vz=0;
            if(enemyAlive_ && enemyHitLockTimer_<=0 && playerHealth_>0) {
                if(distance>.001f)enemyYaw_=std::atan2(dx,dz);
                if(distance>1.45f) {vx=dx/distance*1.15f;vz=dz/distance*1.15f;enemyMoving_=true;}
                else if(enemyAttackCooldown_<=0) {
                    enemyAttackCooldown_=1.15f;enemyAttackRequested_=true;
                    if(playerAttackInvulnerability_<=0 && playerHitInvulnerability_<=0) {
                        playerHealth_=std::max(0.f,playerHealth_-8/inventory_.Stats(pistol_.equipped?0:(sword_.drawn?5:inventory::kNoItem)).defense);
                        playerHitInvulnerability_=kHitInvulnerability;playerHitLockTimer_=kPlayerHitDuration;playerHitFlashTimer_=kHitFlashDuration;
                        AudioManager::GetInstance().PlayAudio("hit",.7f);
                        if(playerHealth_<=0) {
                            playerMoving_=playerSprinting_=false;pistol_.aiming=false;playerDeathHold_=0;playerHitLockTimer_=0;
                            cameraInput_.actionButtonPressedMask=0;EnterPlayerDeathState();
                        }
                    }
                }
            }
            if(enemyAlive_ && physicsWorld_) {
                physicsWorld_->MoveArenaEnemy(i,vx,vz,dt);const auto state=physicsWorld_->GetArenaEnemyState(i);
                enemyX_=state.x;enemyY_=state.y+physics::kPlayerVisualOriginOffset;enemyZ_=state.z;
            }
            enemyMatrix_=Mat4Multiply(Mat4Translation(enemyX_,enemyY_,enemyZ_),Mat4RotationY(enemyYaw_));
            SwapArena(actor);
        }
        int alive=0,visible=0;for(const auto& actor:arenaEnemies_) {alive+=actor.Alive;visible+=actor.Visible;}
        int count=arenaWaves_.Update(dt,alive,visible);if(count)SpawnArenaWave(count);
    }
