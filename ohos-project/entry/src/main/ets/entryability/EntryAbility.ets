import { AbilityConstant,
  common,
  Configuration,
  ConfigurationConstant,
  UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import sdl from 'libSDL3.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    hilog.info(DOMAIN, 'surfaceLayer', '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'surfaceLayer', '%{public}s', 'Ability onDestroy');
  }

  async onWindowStageCreate(windowStage: window.WindowStage): Promise<void> {
    hilog.info(DOMAIN, 'surfaceLayer', '%{public}s', 'Ability onWindowStageCreate');

    try {
      const mainWindow = await windowStage.getMainWindow();
      await mainWindow.setWindowLayoutFullScreen(true);
      await mainWindow.setWindowSystemBarEnable([]);
    } catch (error) {
      hilog.error(DOMAIN, 'surfaceLayer', 'Failed to configure immersive window. Cause: %{public}s', JSON.stringify(error));
    }

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'surfaceLayer', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'surfaceLayer', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, 'surfaceLayer', '%{public}s', 'Ability onWindowStageDestroy');
    sdl.sdlOnTerminate();
  }

  onForeground(): void {
    hilog.info(DOMAIN, 'surfaceLayer', '%{public}s', 'Ability onForeground');
    sdl.sdlOnForeground();
  }

  onBackground(): void {
    hilog.info(DOMAIN, 'surfaceLayer', '%{public}s', 'Ability onBackground');
    sdl.sdlOnBackground();
  }

  onMemoryLevel(level: AbilityConstant.MemoryLevel): void {
    if (level == AbilityConstant.MemoryLevel.MEMORY_LEVEL_LOW || level == AbilityConstant.MemoryLevel.MEMORY_LEVEL_CRITICAL) {
      sdl.sdlOnLowMemory();
    }
  }

  onConfigurationUpdate(newConfig: Configuration): void {
    sdl.sdlOnConfigUpdate();
  }
}
