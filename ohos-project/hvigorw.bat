@rem Copyright (c) 2023 Huawei Device Co., Ltd.
@rem Licensed under the Apache License,Version 2.0 (the "License");
@rem you may not use this file except in compliance with the License.
@rem You may obtain a copy of the License at
@rem
@rem http://www.apache.org/licenses/LICENSE-2.0
@rem
@rem Unless required by applicable law or agreed to in writing, software
@rem distributed under the License is distributed on an "AS IS" BASIS,
@rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
@rem See the License for the specific language governing permissions and
@rem limitations under the License.

@rem
@rem ----------------------------------------------------------------------------
@rem  Hvigor startup script for Windows, version 1.0.0
@rem
@rem  Required ENV vars:
@rem  ------------------
@rem    NODE_HOME - location of a Node home dir
@rem    or
@rem    Add %NODE_HOME%/bin to the PATH environment variable
@rem ----------------------------------------------------------------------------
@rem
@echo off

@rem Set local scope for the variables with Windows NT shell
if "%OS%"=="Windows_NT" setlocal

set "APP_HOME=%~dp0"
for %%i in ("%APP_HOME%") do set "APP_HOME=%%~fi"

@rem DevEco's all-in-one installation supplies the matching Node, Hvigor and JBR.
@rem The project-local hvigor-wrapper.js is not present in this checkout, so use
@rem the installed wrapper directly and keep the SDK root at ...\sdk.
set "DEV_ECO_ROOT=D:\Program Files\Huawei\DevEco Studio"
if defined DEVECO_INSTALL_DIR if exist "%DEVECO_INSTALL_DIR%\tools\hvigor\bin\hvigorw.js" set "DEV_ECO_ROOT=%DEVECO_INSTALL_DIR%"

if exist "%DEV_ECO_ROOT%\tools\node\node.exe" if exist "%DEV_ECO_ROOT%\tools\hvigor\bin\hvigorw.js" goto use_deveco

@rem Fallback for a project-local wrapper when DevEco is installed elsewhere.
if exist "%APP_HOME%hvigor\hvigor-wrapper.js" goto use_project_wrapper

echo.
echo ERROR: DevEco Studio Hvigor was not found and no project-local wrapper exists.
echo Set DEVECO_INSTALL_DIR to the DevEco Studio installation directory.
echo.
exit /b 1

:use_deveco
set "NODE_EXE=%DEV_ECO_ROOT%\tools\node\node.exe"
set "WRAPPER_MODULE_PATH=%DEV_ECO_ROOT%\tools\hvigor\bin\hvigorw.js"
set "JAVA_HOME=%DEV_ECO_ROOT%\jbr"
set "DEVECO_SDK_HOME=%DEV_ECO_ROOT%\sdk"
set "OHOS_BASE_SDK_HOME=%DEV_ECO_ROOT%\sdk"
set "PATH=%JAVA_HOME%\bin;%DEV_ECO_ROOT%\tools\node;%DEV_ECO_ROOT%\tools\ohpm\bin;%DEV_ECO_ROOT%\sdk\default\openharmony\toolchains;%PATH%"
goto execute

:use_project_wrapper
set "NODE_EXE=node.exe"
set "WRAPPER_MODULE_PATH=%APP_HOME%hvigor\hvigor-wrapper.js"
if not defined NODE_OPTS set "NODE_OPTS=--"
goto execute

:execute
if defined NODE_OPTS (
  "%NODE_EXE%" %NODE_OPTS% "%WRAPPER_MODULE_PATH%" %*
) else (
  "%NODE_EXE%" "%WRAPPER_MODULE_PATH%" %*
)
set "EXIT_CODE=%ERRORLEVEL%"
if "%OS%" == "Windows_NT" endlocal & exit /b %EXIT_CODE%
exit /b %EXIT_CODE%
