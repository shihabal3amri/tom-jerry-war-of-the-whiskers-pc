# WOTW Android APK builder — the scripted CLI pipeline (user decision session 28: no Gradle).
# Uses ONLY the local SDK/NDK: cmake (native .so) + aapt2 (resources+manifest) + jar (pack) +
# zipalign + apksigner (debug keystore). Produces a signed, installable debug APK.
#
#   port/tools/build_apk.ps1                 build + package -> port/build-apk/WOTW.apk
#   port/tools/build_apk.ps1 -Install        also `adb install -r` it + push game assets
#   port/tools/build_apk.ps1 -Install -SkipAssets   re-install without re-pushing assets
#   port/tools/build_apk.ps1 -Adb ip:port    connect wireless adb first, then (with -Install)
#
# Output: port/build-apk/WOTW.apk. With -Install, the game data in extracted/ is pushed to
# the app's external files dir (/sdcard/Android/data/com.wotw.port/files/extracted) — the
# stand-in until the SAF ISO picker + on-device extraction land.
param([switch]$Install, [string]$Adb = "", [switch]$SkipNative, [switch]$SkipAssets,
      [switch]$ForceAssets, [switch]$Template)

$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot)          # port/tools -> project root
$sdk  = "$env:LOCALAPPDATA\Android\Sdk"
$bt   = "$sdk\build-tools\34.0.0"
$jbr  = "C:\Program Files\Android\Android Studio\jbr"
$env:JAVA_HOME = $jbr                                   # Android tools want a known-good JDK
$androidJar = "$sdk\platforms\android-34\android.jar"
$cmake = "$sdk\cmake\3.22.1\bin\cmake.exe"
$aapt2 = "$bt\aapt2.exe"
$zipalign = "$bt\zipalign.exe"
$apksigner = "$bt\apksigner.bat"
$jar = "$jbr\bin\jar.exe"
$strip = "$sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"

$appdir = "$root\port\android\app"
$build  = "$root\port\build-apk"
$stage  = "$build\stage"
# ⚠ THIS WIPE DELETES THE INSTALLER'S TEMPLATE. A plain (non -Template) run removed
# WOTW-template.apk, and the next installer build silently kept the last template it had
# -- three commits stale, in the case that prompted this. Carry it across the wipe.
$tplKeep = "$build\WOTW-template.apk"
$tplSave = $null
if (Test-Path $tplKeep) { $tplSave = "$env:TEMP\WOTW-template.keep.apk"; Copy-Item $tplKeep $tplSave -Force }
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\lib\arm64-v8a" | Out-Null
if ($tplSave) { Copy-Item $tplSave $tplKeep -Force }   # ...and put it back

# 1) native binaries ------------------------------------------------------------------------
# libtjapp.so  = the compositor shell (NativeActivity).
# libtjgame.so = the GAME — an ART-free PIE executable named like a library so the installer
#                extracts it to the native-lib dir, the one app path exec() is allowed from.
if (-not $SkipNative) {
    Write-Host "== building libtjapp.so + libtjgame.so (arm64-v8a) =="
    & $cmake --build "$root\port\build-arm64" --target tjapp tjgame
    if ($LASTEXITCODE -ne 0) { throw "native build failed" }
}
foreach ($lib in @("libtjapp.so", "libtjgame.so")) {
    $so = "$root\port\build-arm64\$lib"
    if (-not (Test-Path $so)) { throw "$lib not found ($so)" }
    Copy-Item $so "$stage\lib\arm64-v8a\$lib" -Force
    if (Test-Path $strip) { & $strip "$stage\lib\arm64-v8a\$lib" }   # shrink (debug info out)
}

# 2) resources + manifest -> base apk -------------------------------------------------------
Write-Host "== aapt2 compile + link =="
& $aapt2 compile --dir "$appdir\res" -o "$build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 compile failed" }
& $aapt2 link -o "$build\app-unsigned.apk" -I $androidJar `
    --manifest "$appdir\AndroidManifest.xml" `
    --min-sdk-version 26 --target-sdk-version 29 $(if ($Template) { "--no-compress" }) `
    "$build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

# 3) add the native libs (extractNativeLibs=true, so compression is fine) --------------------
Write-Host "== packing native libs =="
# -0 for the template: the INSTALLER re-signs the apk after adding the player's game data,
# and a v1 (JAR) signature digests every entry's UNCOMPRESSED bytes. Storing everything means
# the installer never needs an inflate implementation to hash what it copies - it is byte
# copying and SHA-256, nothing more. Costs ~600 KB in a file about to gain 223 MB of game data.
$jarFlags = if ($Template) { "u0f" } else { "uf" }
& $jar $jarFlags "$build\app-unsigned.apk" -C $stage "lib/arm64-v8a/libtjapp.so" -C $stage "lib/arm64-v8a/libtjgame.so"
if ($LASTEXITCODE -ne 0) { throw "jar add failed" }

# 3b) TEMPLATE MODE: an UNSIGNED, all-STORED apk with NO game data, for the Windows installer
# to embed. The installer adds the player's own assets/game.pak and writes its own v1
# signature, so signing here would only be thrown away. This is the artifact that lets the
# installer build a self-contained apk with NO Android SDK on the player's machine.
if ($Template) {
    $tpl = "$build\WOTW-template.apk"
    # aapt2 honours --no-compress for resources but ALWAYS deflates AndroidManifest.xml;
    # this pass makes every entry STORED so the installer needs no inflate to hash them.
    & python "$root\port\tools\apk_store_all.py" "$build\app-unsigned.apk" $tpl
    if ($LASTEXITCODE -ne 0) { throw "apk_store_all failed" }
    Write-Host "TEMPLATE: $tpl"
    return
}

# 4) align + sign (debug keystore) ----------------------------------------------------------
Write-Host "== zipalign + apksigner =="
& $zipalign -f -p 4 "$build\app-unsigned.apk" "$build\app-aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }
# SIGN WITH THE INSTALLER'S KEY WHEN THE PLAYER HAS ONE. The installer generates a per-user
# key at %LOCALAPPDATA%\TomJerryWOW\android-signing.pfx and signs its self-contained apk
# with it. Android refuses to update an app whose signing certificate changed, so a dev
# build signed with the debug keystore CANNOT install over an installer-built one --
# INSTALL_FAILED_UPDATE_INCOMPATIBLE, and the only way out is uninstalling and re-pushing
# 223 MB of assets. Sharing the identity makes the two interchangeable.
$playerKey = "$env:LOCALAPPDATA\TomJerryWOW\android-signing.pfx"
$ks = "$env:USERPROFILE\.android\debug.keystore"
$ksPass = "pass:android"
$ksAlias = @("--ks-key-alias", "androiddebugkey")
if (Test-Path $playerKey) {
    $ks = $playerKey
    $ksPass = "pass:tjwow"
    $ksAlias = @()          # a PFX has one entry; apksigner finds it without an alias
    Write-Host "   signing with the installer key ($playerKey)"
}
& $apksigner sign --ks $ks --ks-pass $ksPass @ksAlias `
    --out "$build\WOTW.apk" "$build\app-aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Write-Host "== verifying =="
& $apksigner verify --print-certs "$build\WOTW.apk" | Select-Object -First 3
& $aapt2 dump badging "$build\WOTW.apk" | Select-String "package:|application-label:|native-code|sdkVersion|uses-feature"
Write-Host "APK: $build\WOTW.apk"

# 5) optional install + game assets ---------------------------------------------------------
if ($Install) {
    $adbExe = "$sdk\platform-tools\adb.exe"
    if ($Adb -ne "") { & $adbExe connect $Adb }
    & $adbExe install -r "$build\WOTW.apk"
    if (-not $SkipAssets) {
        # Push the game data to the app's external files dir. Stand-in for the SAF picker +
        # on-device extraction (still no disc data in the APK — this is the user's own copy).
        #
        # ⚠ adb push SEMANTICS (verified ON THIS DEVICE by the session-28 review): pushing a
        # LOCAL DIR at an EXISTING remote dir NESTS it (basename-append) — so pushes go to
        # the PARENT ($extDir), where the append resolves to files/extracted and retries
        # MERGE instead of nesting. ⚠ PS 5.1 + $ErrorActionPreference=Stop: never redirect
        # adb's stderr (2>$null throws NativeCommandError) — gate on test/echo instead.
        #
        # ⚠ _SAVES SAFETY: assets never change, so the push is SKIPPED when the device has
        # them. -ForceAssets re-pushes, but: device _SAVES is backed up first (pull VERIFIED
        # non-empty before anything destructive), the remote tree is then replaced, the
        # PC's own _SAVES content is REMOVED from the device copy, and the device backup is
        # restored per-entry (per-entry, so basename-append can't nest junk INSIDE _SAVES).
        $extDir = "/sdcard/Android/data/com.wotw.port/files"
        $have = & $adbExe shell "test -f $extDir/extracted/default.xbe && echo yes || echo no"
        if ("$have" -match "yes" -and -not $ForceAssets) {
            Write-Host "== game assets already on device; skipping push (use -ForceAssets to re-push) =="
        } elseif ("$have" -notmatch "yes") {
            Write-Host "== pushing game assets to $extDir/extracted (~223 MB) =="
            & $adbExe shell mkdir -p "$extDir"
            & $adbExe push "$root\extracted" "$extDir"     # parent target: lands at files/extracted
        } else {
            # -ForceAssets over an existing tree.
            $bk = "$env:TEMP\tj_device_saves_backup"
            $haveSaves = & $adbExe shell "test -d $extDir/extracted/_SAVES && echo yes || echo no"
            if ("$haveSaves" -match "yes") {
                Write-Host "== backing up device _SAVES to $bk =="
                Remove-Item $bk -Recurse -Force -ErrorAction SilentlyContinue
                & $adbExe pull "$extDir/extracted/_SAVES" $bk
                if (-not (Test-Path $bk)) { throw "_SAVES backup pull FAILED - aborting before any delete" }
            }
            Write-Host "== replacing device assets (~223 MB) =="
            & $adbExe shell "rm -rf $extDir/extracted"     # gated: backup verified above
            & $adbExe push "$root\extracted" "$extDir"
            # The PC's _SAVES content must never masquerade as the device's saves.
            & $adbExe shell "rm -rf $extDir/extracted/_SAVES && mkdir -p $extDir/extracted/_SAVES"
            if ("$haveSaves" -match "yes") {
                Write-Host "== restoring device _SAVES =="
                Get-ChildItem $bk | ForEach-Object {
                    & $adbExe push $_.FullName "$extDir/extracted/_SAVES/$($_.Name)"
                }
                Write-Host "== device _SAVES after restore =="
                & $adbExe shell "ls $extDir/extracted/_SAVES"
            }
        }
    }
    # THE ARABIC PACK rides with the game data, not in the apk: on Android the executable is
    # libtjgame.so in the app's read-only native-lib dir, so arabic.cpp looks in the ASSET ROOT
    # instead. Pushed to the root of extracted/, which is LAN-safe -- dataHash covers
    # default.xbe plus GFX/AUDMUSIC/AUDSoundFX, so a peer without the pack still matches.
    $pack = "$root\portuild-arabicrabic_font.bin"
    if (Test-Path $pack) {
        & $adbExe push $pack "$extDir/extracted/arabic_font.bin"
        Write-Host "== pushed arabic_font.bin ($([math]::Round((Get-Item $pack).Length/1KB)) KB) =="
    } else {
        Write-Host "== no arabic_font.bin (run tools/arabic_font.py --pack) -- APK stays English =="
    }
    Write-Host "launching..."
    & $adbExe shell am start -n com.wotw.port/android.app.NativeActivity
}
