import { webview } from '@kit.ArkWeb';
import { hilog } from '@kit.PerformanceAnalysisKit';
import sdl from 'libSDL3.so';
import { intl } from '@kit.LocalizationKit';
import { deviceInfo, pasteboard } from '@kit.BasicServicesKit';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { batteryInfo } from '@kit.BasicServicesKit';

import  { picker } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0x0000;
let context: UIContext

export class ArkNapiCallback {
  onMainLaunch() {
    sdl.sdlLaunchMain("libentry.so", "main")
  }

  // Called from the native SDL thread right after the first frame is
  // presented: the engine content is on screen, drop the splash overlay.
  hideNativeSplash() {
    hilog.info(DOMAIN, 'surfaceLayer', 'First frame presented, hiding splash.')
    AppStorage.setOrCreate('splashHidden', true)
  }

  fetchLocale(): string {
    let locale = new intl.Locale()
    return locale.language + "_" + locale.region
  }
  firstCall() {
    hilog.info(DOMAIN, 'surfaceLayer', 'Native binding finished.')
    sdl.sdlOnConfigUpdate();
  }

  openLink(url: string) {
    if (url === 'mikan://html-game') {
      sdl.sdlOnBackground()
      AppStorage.setOrCreate('htmlGameVisible', true)
      return
    }
    let con = context.getHostContext() as common.UIAbilityContext
    con.openLink(url)
  }

  hasBattery(): number {
    return batteryInfo.isBatteryPresent ? 1 : 0
  }
  batteryCharging(): number {
    return batteryInfo.chargingStatus == batteryInfo.BatteryChargeState.ENABLE ? 1 : 0
  }
  batteryCharged(): number {
    return batteryInfo.chargingStatus == batteryInfo.BatteryChargeState.FULL ? 1 : 0
  }
  batteryPercent(): number {
    return batteryInfo.batterySOC
  }

  setPasteboardString(text: string) {
    let pasteData: pasteboard.PasteData = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
    let systemPasteboard: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
    systemPasteboard.setData(pasteData, (err, data) => {
      if (err) {
        hilog.error(DOMAIN, 'surfaceLayer', 'Failed to set PasteData. Cause: ' + err.message);
        return;
      }

      hilog.info(DOMAIN, 'surfaceLayer', 'Succeed in setting PasteData.');
    });
  }

  textEditing(): number {
    return 0;
  }

  startTextInput() {
    // The Vulkan sample has no editable UI and must not open the IME.
  }
  stopTextInput() {
    // Keep the SDL callback surface while leaving the IME disabled.
  }

  openFileDialog(id: number, defpath: string, itemcount: number, filter: string) {
    const documentSelectOptions = new picker.DocumentSelectOptions();
    documentSelectOptions.maxSelectNumber = itemcount;
    documentSelectOptions.defaultFilePathUri = defpath;

    if (id == 0 || id == 1)
    {
      documentSelectOptions.selectMode = picker.DocumentSelectMode.FILE;
    }
    if (id == 2)
    {
      documentSelectOptions.selectMode = picker.DocumentSelectMode.FOLDER;
    }

    let targetfilter: string[] = []
    let temp = filter.split('\u0002')
    for (let index = 0; index < temp.length; index++) {
      const element = temp[index];

      let temp2 = element.split("|")

      // broken filter!
      if (temp2.length == 1)
      {
        targetfilter.push(element + "|.*")
      }
      else
      {
        let targettemp = temp2[0] + "|";

        let temp3 = temp2[1].split(";")
        for (let index1 = 0; index1 < temp3.length; index1++) {
          targettemp += "." + temp3[index1] + ",";
        }
        targetfilter.push(targettemp)
      }
    }

    documentSelectOptions.fileSuffixFilters = targetfilter;
    documentSelectOptions.authMode = false;
    documentSelectOptions.mergeMode = picker.MergeTypeMode.DEFAULT;
    documentSelectOptions.isEncryptionSupported = false;

    let uris: string[] = [];
    let contextt = context.getHostContext() as common.UIAbilityContext;
    const documentViewPicker = new picker.DocumentViewPicker(contextt);
    documentViewPicker.select(documentSelectOptions).then((documentSelectResult: Array<string>) => {
      uris = documentSelectResult;
      sdl.sdlDialogClearSelection()
      for (let idx = 0; idx < uris.length; idx++)
      {
        let target = uris[idx].replace("file://docs", "")
        sdl.sdlDialogFileSelected(target)
        console.info('documentViewPicker.select to file succeed and uris are: ' + target);
      }
      sdl.sdlDialogExecCallback()
    }).catch((err: BusinessError) => {
      console.error(`Invoke documentViewPicker.select failed, code is ${err.code}, message is ${err.message}`);
    })
  }

  getInternalPath(): string {
    let contextt = context.getHostContext() as common.UIAbilityContext
    return contextt.filesDir
  }

  themeIsDark(): number {
    let contextt = context.getHostContext() as common.UIAbilityContext
    return (contextt.config.colorMode == ConfigurationConstant.ColorMode.COLOR_MODE_DARK) ? 1 : 0;
  }

  isPhone(): number {
    return deviceInfo.deviceType == "phone" ? 1 : 0;
  }
}

let callbackRef: ArkNapiCallback = new ArkNapiCallback()

@Entry
@Component
struct Index {
  @StorageProp('splashHidden') splashHidden: boolean = false
  @StorageLink('htmlGameVisible') webVisible: boolean = false
  @State webProgress: number = 0
  @State webWidth: number = 640
  @State webHeight: number = 320
  private webController: webview.WebviewController = new webview.WebviewController()

  private openWeb(): void {
    sdl.sdlOnBackground()
    this.webVisible = true
  }

  private closeWeb(): void {
    this.webVisible = false
    sdl.sdlOnForeground()
  }

  onBackPress(): boolean {
    if (this.webVisible) {
      this.closeWeb()
      return true
    }
    return false
  }

  aboutToAppear(): void {
    context = this.getUIContext()

    const resMgr = (context.getHostContext() as common.UIAbilityContext).resourceManager
    sdl.sdlResourceManagerInit(resMgr)

    sdl.sdlCallbackInit(callbackRef)
  }

  build() {
    Stack() {
      XComponent({ id: 'mainView', type: 'surface', libraryname: 'SDL3' })
        .id('mainView')
        .hitTestBehavior(this.webVisible ? HitTestMode.None : HitTestMode.Default)
        .width('100%')
        .height('100%')

      if (this.webVisible) {
        Column() {
          Column() {
            Row({ space: 10 }) {
              Text('实验 · HTML 小游戏').fontColor(Color.White).fontSize(16)
              Blank()
              Button('关闭').onClick(() => this.closeWeb())
            }.width('100%').padding(10).backgroundColor('#172438')
            if (this.webProgress < 100) {
              Progress({ value: this.webProgress, total: 100, type: ProgressType.Linear })
                .width('100%').height(3)
            }
            Web({ src: $rawfile('html/index.html'), controller: this.webController,
              renderMode: RenderMode.ASYNC_RENDER })
              .hitTestBehavior(HitTestMode.Default)
              .onControllerAttached(() => {
                this.webController.setCustomUserAgent(
                  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36')
              })
              .onConsole((event) => {
                if (event) hilog.info(DOMAIN, 'HtmlGame', '%{public}s', event.message.getMessage())
                return false
              })
              .javaScriptAccess(true)
              .domStorageAccess(true)
              .fileAccess(false)
              .mixedMode(MixedMode.None)
              .onProgressChange((event) => {
                if (event) this.webProgress = event.newProgress
              })
              .width(this.webWidth).height(this.webHeight)
          }.width('92%').height('88%').backgroundColor('#101c2d')
            // The emulator's ArkWeb output failed with weighted/fractional surfaces
            // (40001000, red repeated tiles). Use explicit pixel-aligned dimensions
            // and avoid clipping the Web surface. Verified by screenshots and swipes.
            .onAreaChange((_oldArea, area) => {
              this.webWidth = px2vp(Math.floor(vp2px(Number(area.width)) / 64) * 64)
              this.webHeight = px2vp(Math.floor(vp2px(Math.max(100, Number(area.height) - 64)) / 64) * 64)
            })
        }.width('100%').height('100%').backgroundColor('#99000000')
          .justifyContent(FlexAlign.Center).hitTestBehavior(HitTestMode.Default)
      }

      // Splash overlay: stays on top of the (possibly black) surface until
      // the engine presents its first frame, then hideNativeSplash() flips
      // the AppStorage flag and this branch goes away.
      if (!this.splashHidden) {
        Column() {
          Image($r('app.media.mikan_engine_splash'))
            .width('100%')
            .objectFit(ImageFit.Cover)
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#FCFBF0')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .onKeyEvent((keyevent) => {
      if (!this.webVisible) sdl.sdlKeyEvent(keyevent.keyCode, keyevent.type);
    })
  }
}
