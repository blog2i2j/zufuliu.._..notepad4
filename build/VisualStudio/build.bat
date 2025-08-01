@ECHO OFF
@rem ******************************************************************************
@rem *
@rem * Notepad4
@rem *
@rem * build.bat
@rem *   Batch file used to build Notepad4 with MSVC 2019, 2022
@rem *
@rem * See License.txt for details about distribution and modification.
@rem *
@rem *                                     (c) XhmikosR 2010-2015, 2017
@rem *                                     https://github.com/XhmikosR/Notepad2-mod
@rem *
@rem ******************************************************************************

SETLOCAL ENABLEEXTENSIONS
CD /D %~dp0

@rem Check for the help switches
IF /I "%~1" == "help"   GOTO SHOWHELP
IF /I "%~1" == "/?"     GOTO SHOWHELP

@rem default arguments
SET "BUILDTYPE=Build"
SET "ARCH=all"
SET NO_32BIT=0
SET "CONFIG=Release"

@rem Check for the first switch
IF "%~1" == "" GOTO StartWork
IF /I "%~1" == "Build"     SET "BUILDTYPE=Build"   & SHIFT & GOTO CheckSecondArg
IF /I "%~1" == "Clean"     SET "BUILDTYPE=Clean"   & SHIFT & GOTO CheckSecondArg
IF /I "%~1" == "Rebuild"   SET "BUILDTYPE=Rebuild" & SHIFT & GOTO CheckSecondArg


:CheckSecondArg
@rem Check for the second switch
IF "%~1" == "" GOTO StartWork
IF /I "%~1" == "x86"     SET "ARCH=Win32" & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "Win32"   SET "ARCH=Win32" & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "x64"     SET "ARCH=x64"   & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "AVX2"    SET "ARCH=AVX2"  & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "AVX512"  SET "ARCH=AVX512" & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "ARM64"   SET "ARCH=ARM64" & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "all"     SET "ARCH=all"   & SHIFT & GOTO CheckThirdArg
IF /I "%~1" == "No32bit"   SET "ARCH=all" & SET NO_32BIT=1 & SHIFT & GOTO CheckThirdArg


:CheckThirdArg
@rem Check for the third switch
IF "%~1" == "" GOTO StartWork
IF /I "%~1" == "Debug"         SET "CONFIG=Debug"       & SHIFT & GOTO StartWork
IF /I "%~1" == "Release"       SET "CONFIG=Release"     & SHIFT & GOTO StartWork
IF /I "%~1" == "LLVMDebug"     SET "CONFIG=LLVMDebug"   & SHIFT & GOTO StartWork
IF /I "%~1" == "LLVMRelease"   SET "CONFIG=LLVMRelease" & SHIFT & GOTO StartWork
IF /I "%~1" == "all"           SET "CONFIG=all"         & SHIFT & GOTO StartWork


:StartWork
SET "EXIT_ON_ERROR=%~1"

SET NEED_ARM64=0
IF /I "%ARCH%" == "all" SET NEED_ARM64=1
IF /I "%ARCH%" == "ARM64" SET NEED_ARM64=1
CALL :SubVSPath
IF NOT EXIST "%VS_PATH%" CALL :SUBMSG "ERROR" "Visual Studio 2019 or 2022 NOT FOUND, please check VS_PATH environment variable!"

IF /I "%processor_architecture%" == "AMD64" (
	SET "HOST_ARCH=amd64"
) ELSE (
	SET "HOST_ARCH=x86"
)

IF %NO_32BIT% == 1 GOTO x64
IF /I "%ARCH%" == "AVX2" GOTO AVX2
IF /I "%ARCH%" == "AVX512" GOTO AVX512
IF /I "%ARCH%" == "x64" GOTO x64
IF /I "%ARCH%" == "Win32" GOTO Win32
IF /I "%ARCH%" == "ARM64" GOTO ARM64


:Win32
SETLOCAL
CALL "%VS_PATH%\Common7\Tools\vsdevcmd" -no_logo -arch=x86 -host_arch=%HOST_ARCH%
IF /I "%CONFIG%" == "all" (CALL :SUBMSVC %BUILDTYPE% Debug Win32 && CALL :SUBMSVC %BUILDTYPE% Release Win32) ELSE (CALL :SUBMSVC %BUILDTYPE% %CONFIG% Win32)
ENDLOCAL
IF /I "%ARCH%" == "Win32" GOTO END


:x64
SETLOCAL
CALL "%VS_PATH%\Common7\Tools\vsdevcmd" -no_logo -arch=amd64 -host_arch=%HOST_ARCH%
IF /I "%CONFIG%" == "all" (CALL :SUBMSVC %BUILDTYPE% Debug x64 && CALL :SUBMSVC %BUILDTYPE% Release x64) ELSE (CALL :SUBMSVC %BUILDTYPE% %CONFIG% x64)
ENDLOCAL
IF /I "%ARCH%" == "x64" GOTO END


:AVX2
SETLOCAL
CALL "%VS_PATH%\Common7\Tools\vsdevcmd" -no_logo -arch=amd64 -host_arch=%HOST_ARCH%
IF /I "%CONFIG%" == "all" (CALL :SUBMSVC %BUILDTYPE% AVX2Debug x64 && CALL :SUBMSVC %BUILDTYPE% AVX2Release x64) ELSE (CALL :SUBMSVC %BUILDTYPE% AVX2%CONFIG% x64)
ENDLOCAL
IF /I "%ARCH%" == "AVX2" GOTO END


:AVX512
SETLOCAL
CALL "%VS_PATH%\Common7\Tools\vsdevcmd" -no_logo -arch=amd64 -host_arch=%HOST_ARCH%
IF /I "%CONFIG%" == "all" (CALL :SUBMSVC %BUILDTYPE% AVX512Debug x64 && CALL :SUBMSVC %BUILDTYPE% AVX512Release x64) ELSE (CALL :SUBMSVC %BUILDTYPE% AVX512%CONFIG% x64)
ENDLOCAL
IF /I "%ARCH%" == "AVX512" GOTO END


:ARM64
SETLOCAL
CALL "%VS_PATH%\Common7\Tools\vsdevcmd" -no_logo -arch=arm64 -host_arch=%HOST_ARCH%
IF /I "%CONFIG%" == "all" (CALL :SUBMSVC %BUILDTYPE% Debug ARM64 && CALL :SUBMSVC %BUILDTYPE% Release ARM64) ELSE (CALL :SUBMSVC %BUILDTYPE% %CONFIG% ARM64)
ENDLOCAL
IF /I "%ARCH%" == "ARM64" GOTO END


:END
TITLE Building Notepad4 with MSVC - Finished!
ENDLOCAL
EXIT /B


:SubVSPath
@rem Check the building environment
@rem VSINSTALLDIR is set by vsdevcmd_start.bat
IF EXIST "%VSINSTALLDIR%\Common7\IDE\VC\VCTargets\Platforms\%ARCH%\PlatformToolsets" (
	SET "VS_PATH=%VSINSTALLDIR%"
	EXIT /B
)

SET VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
SET "VS_COMPONENT=Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
IF "%NEED_ARM64%" == 1 SET "VS_COMPONENT=%VS_COMPONENT% Microsoft.VisualStudio.Component.VC.Tools.ARM64"
FOR /f "delims=" %%A IN ('"%VSWHERE%" -property installationPath -prerelease -version [16.0^,18.0^) -requires %VS_COMPONENT%') DO SET "VS_PATH=%%A"
IF EXIST "%VS_PATH%" (
	SET "VSINSTALLDIR=%VS_PATH%\"
	EXIT /B
)
@rem Visual Studio Build Tools
FOR /f "delims=" %%A IN ('"%VSWHERE%" -products Microsoft.VisualStudio.Product.BuildTools -property installationPath -prerelease -version [16.0^,18.0^) -requires %VS_COMPONENT%') DO SET "VS_PATH=%%A"
IF EXIST "%VS_PATH%" SET "VSINSTALLDIR=%VS_PATH%\"
EXIT /B


:SUBMSVC
ECHO.
TITLE Building Notepad4 with MSVC - %~1 "%~2|%~3"...
CD /D %~dp0
"MSBuild.exe" /nologo Notepad4.sln /target:Notepad4;%~1 /property:Configuration=%~2;Platform=%~3^ /consoleloggerparameters:Verbosity=minimal /maxcpucount /nodeReuse:true
IF %ERRORLEVEL% NEQ 0 CALL :SUBMSG "ERROR" "Compilation failed!"
EXIT /B


:SHOWHELP
TITLE %~nx0 %1
ECHO. & ECHO.
ECHO Usage: %~nx0 [Clean^|Build^|Rebuild] [Win32^|x64^|AVX2^|AVX512^|ARM64^|all^|No32bit] [Debug^|Release^|LLVMDebug^|LLVMRelease^|all]
ECHO.
ECHO Notes: You can also prefix the commands with "-", "--" or "/".
ECHO        The arguments are not case sensitive.
ECHO. & ECHO.
ECHO Executing %~nx0 without any arguments is equivalent to "%~nx0 build all release"
ECHO.
ECHO If you skip the second argument the default one will be used.
ECHO The same goes for the third argument. Examples:
ECHO "%~nx0 rebuild" is the same as "%~nx0 rebuild all release"
ECHO "%~nx0 rebuild Win32" is the same as "%~nx0 rebuild Win32 release"
ECHO.
ECHO WARNING: "%~nx0 Win32" or "%~nx0 debug" won't work.
ECHO.
ENDLOCAL
EXIT /B


:SUBMSG
ECHO. & ECHO ______________________________
ECHO [%~1] %~2
ECHO ______________________________ & ECHO.
IF /I "%~1" == "ERROR" (
  IF "%EXIT_ON_ERROR%" == "" PAUSE
  EXIT /B
) ELSE (
  EXIT /B
)
