# Project Grand Tourer Advanced

A native Windows PC port of **Gran Turismo 6** (PlayStation 3, Polyphony Digital / Sony Interactive Entertainment, European disc **BCES01893**, update **1.22**).

- The game's PPU code is statically recompiled to C++ with [ps3recomp](https://github.com/sp00nznet/ps3recomp). No emulator runs the game.
- RSX graphics are drawn with Direct3D 12.
- The launcher prepares everything from your own disc with one button and keeps itself up to date.

> **The game is not included.** You need your own copy of the European disc (BCES01893), as a decrypted ISO or as a folder copied from the disc.
> All rights to Gran Turismo 6 and to the data and code in it belong to Sony Interactive Entertainment and Polyphony Digital. This is a non-profit fan project. It is not affiliated with or endorsed by them, and this repository contains no game data.

[한국어 안내는 아래에 있습니다.](#한국어)

## Supported

- **Windows 10 / 11, 64-bit PCs only.** Steam Deck, Android, macOS, iOS and Linux are not supported, and support for them is not planned.
- **One region only: the European disc BCES01893** with its official update 1.22 (the launcher downloads it). Other regions are not supported, and support for them is not planned.
- **Frame rate:** it will keep being improved, but that is a long road.

## Features

- **One-click setup** (launcher, **Install** page). The launcher:
  - finds your disc image or disc folder by itself (or you pick it), and unpacks the disc's files;
  - downloads the game's official updates 1.01 - 1.22 (about 12.5 GB) and the PS3 system software from Sony's servers, and the official RPCS3 build from its GitHub releases;
  - installs and checks everything (SHA-256 against the files the port was built from) and finishes the update merge the game itself does on its first start.

  RPCS3 is used only as a tool during the setup and is deleted afterwards. Everything happens out of sight; only a progress bar is shown. If the setup stops, starting it again continues where it stopped.
- **In-game menu (F2):** credit lock, career and licence unlock, PP lock, and settings, in English or Korean.
- **LAN mode** (F2 menu, Network): race other PCs on the same network with the game's own hidden LAN mode, no PSN needed. **Looser chase camera** (F2 menu, Display), with its two values.
- **Launcher:** settings (Full HD and more), mods, save backups, logs, and automatic updates of the port.
- **Mods:** the GT6 Career Mod with the Spec II mod is imported in one step. Mods never change the game data: the game reads them first, so switching them off gives the original game at once.
- **Languages:** English and Korean (by Windows' language, or chosen in the launcher).

## Download and install

1. Download the zip of the latest release from the [Releases](https://github.com/PrimeTimerer/Project-Grand-Tourer-Advanced/releases) page and unzip it into an empty folder. The setup needs about 45 GB of free space while it runs; the finished installation uses about 29 GB.
2. Run `GT6 Launcher.exe`. Without game files it opens on **Install**. It looks for your disc by itself; if it does not find it, pick the ISO file or the disc folder. Press **Start setup**.
   - Downloading depends on your connection (about 20 minutes at 100 Mbps). Installing takes about 15 minutes.
   - No other window opens. You can do something else meanwhile.
3. Press **Play**.

**The first start:**
- Screen Adjust starts at 90 %. Hold Up to set it, then confirm.
- When the game asks for your name, a Windows input box opens. Type the name and press Enter.
- The first drive is a track day at Brands Hatch. Drive it to the end; then the main screen (My Home) comes up and the save is made.

The launcher holds no Sony keys. RPCS3 decrypts the game's executable and one system module during the setup; nothing of it is kept, and every result is checked against the hash of the file the port was built from.

## Mods

**GT6 Career Mod (+ Spec II mod)** by Lunard, based on Spec II Beta 7 (Nenkai, TheAdmiester), with features from Strato mod (Emivebe) and Performance mod (Plunke):

1. Start the game once, play to the main screen (My Home) so that the save is made, and close the game.
2. Download "GT6 Career mod ver. ….zip" from its [GTPlanet page](https://www.gtplanet.net/forum/downloads/gran-turismo-6-career-mod.7049/).
3. In the launcher: **Mods → Import archive…**, pick the zip. Spec II and the Career mod are switched on in the right order.

Start a new save when you first use the Career mod. This project never downloads, bundles or re-hosts other people's mods.

## Updates

The launcher checks this repository's releases once a day. A newer release is downloaded in the background and applied the next time the launcher opens. Game files, saves, settings and mods stay as they are.

## Build

[BUILD.md](BUILD.md) has the steps. In short: apply `patches/*.patch` to the `ps3recomp` submodule in number order, prepare `game\` from your own disc with the launcher's setup, lift `game\EMAIN.ELF` with ps3recomp's PPU lifter, then run `build_release.bat` and `build_launcher.bat`.

## Credits

- **[RPCS3](https://github.com/RPCS3/rpcs3)**, the PlayStation 3 emulator. The launcher uses its official build during the setup to decrypt the game's executable and one firmware module, and deletes it afterwards. The build turns its community patches for GT6 1.22 (AlexHaust, illusion and others) into the port's switches, and RPCS3's work on how the PS3 runs Gran Turismo 6 was the reference for this port.
- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** by sp00nznet: the static recompiler and runtime the port is built on.
- **[MABEC](https://github.com/Mabec-games/Gran-Turismo-6-PC)** (Gran Turismo 6 PC, a GT6 build for RPCS3): its LAN play without PSN - the hidden "lanmode" boot argument with the "LAN Room Sync Without PSN" patch by abowen and RPCS3's "Develop Settings" - is the base of the port's LAN mode; its notes also pointed to the "Looser Chase Camera" patch by Edo (the port's looser chase camera) and to the game's own way of setting credits. No code of it is used; the facts are, with thanks.
- **[Dear ImGui](https://github.com/ocornut/imgui)**: the in-game menu.

## License

- The port's own code and files are under the [MIT License](LICENSE).
- `third_party/imgui` (Dear ImGui) and the `ps3recomp` submodule keep their own MIT licenses. The release contains SDL2 (zlib license) and the Visual C++ runtime DLLs.
- RPCS3 is not part of this repository or its releases. The launcher downloads the official build during the setup and deletes it afterwards.
- `gt6_patches.inc` names game patches by the authors of their RPCS3 patches (AlexHaust, illusion and others); those patches are their work.
- The license does not cover the game or anything taken from it; those belong to Sony Interactive Entertainment and Polyphony Digital. This includes the recompiled game code inside the release exe.

---

## 한국어

**그란 투리스모 6**(PlayStation 3, Polyphony Digital / Sony Interactive Entertainment, 유럽판 디스크 **BCES01893**, 업데이트 **1.22**)의 Windows PC 네이티브 포트입니다.

- 게임의 PPU 코드를 [ps3recomp](https://github.com/sp00nznet/ps3recomp)로 C++로 정적 리컴파일했습니다. 에뮬레이터가 게임을 돌리지 않습니다.
- RSX 그래픽은 Direct3D 12로 그립니다.
- 런처가 사용자의 디스크에서 버튼 하나로 모든 것을 준비하고, 스스로 업데이트합니다.

> **게임은 포함되어 있지 않습니다.** 본인의 유럽판 디스크(BCES01893)가 필요합니다. 복호화된 ISO나 디스크에서 복사한 폴더로 준비해 주세요.
> 그란 투리스모 6과 그 안의 데이터 · 코드에 대한 모든 권리는 Sony Interactive Entertainment와 Polyphony Digital에 있습니다. 이 프로젝트는 비영리 팬 프로젝트이며 그들과 관계가 없고, 이 저장소에는 게임 데이터가 없습니다.

### 지원 범위

- **Windows 10 / 11 64비트 PC 전용입니다.** Steam Deck, 안드로이드, macOS, iOS, 리눅스는 지원하지 않으며 지원할 예정도 없습니다.
- **한 지역판만 지원합니다: 유럽판 디스크 BCES01893**과 공식 업데이트 1.22(런처가 받아 줍니다). 다른 지역판은 지원하지 않으며 지원할 예정도 없습니다.
- **프레임:** 앞으로도 계속 개선해 나가겠지만, 긴 여정이 될 것입니다.

### 기능

- **원클릭 설치**(런처의 **설치** 페이지): 디스크 이미지나 디스크 폴더를 스스로 찾고(또는 직접 고르고) 디스크의 파일을 꺼냅니다. 게임의 공식 업데이트 1.01~1.22(약 12.5 GB)와 PS3 시스템 소프트웨어를 소니 서버에서, 공식 RPCS3 빌드를 RPCS3의 GitHub 릴리스에서 받습니다. 모두 설치하고 검사한 뒤(포트를 만든 원본 파일과 SHA-256 비교), 게임이 첫 실행 때 하는 업데이트 합치기까지 끝냅니다. RPCS3는 설치하는 동안 도구로만 쓰고 끝나면 지웁니다. 모든 과정은 보이지 않는 곳에서 진행되고 진행 바만 보입니다. 중간에 멈춰도 다시 시작하면 멈춘 곳부터 이어집니다.
- **게임 안 메뉴(F2):** 크레딧 고정, 커리어·라이선스 해제, PP 고정과 설정, 영어 또는 한국어.
- **LAN 모드**(F2 메뉴의 네트워크): 게임 자체의 숨은 LAN 모드로 같은 네트워크의 PC끼리 대전합니다(PSN 필요 없음). **느슨한 추적 카메라**(F2 메뉴의 화면)와 그 두 값.
- **런처:** 설정(Full HD 등), 모드, 세이브 백업, 로그, 포트 자동 업데이트.
- **모드:** GT6 커리어 모드와 Spec II 모드를 한 번에 가져옵니다. 모드는 게임 데이터를 바꾸지 않고 게임이 먼저 읽게만 하므로, 끄면 바로 원래 게임입니다.
- **언어:** 영어와 한국어(Windows 언어를 따르거나 런처에서 선택).

### 받기와 설치

1. [Releases](https://github.com/PrimeTimerer/Project-Grand-Tourer-Advanced/releases) 페이지에서 최신 릴리스의 zip을 받아 빈 폴더에 풉니다. 설치하는 동안 약 45 GB의 여유 공간이 필요하고, 설치가 끝나면 약 29 GB를 씁니다.
2. `GT6 Launcher.exe`를 실행합니다. 게임 파일이 없으면 **설치** 페이지가 열립니다. 디스크를 스스로 찾고, 못 찾으면 ISO 파일이나 디스크 폴더를 고르세요. **설치 시작**을 누릅니다.
   - 받기는 인터넷 속도에 따라 다릅니다(100 Mbps에서 약 20분). 설치는 약 15분입니다.
   - 다른 창은 뜨지 않습니다. 그동안 다른 일을 하셔도 됩니다.
3. **플레이**를 누릅니다.

**처음 시작할 때:**
- 화면 조정(Screen Adjust)은 90%에서 시작합니다. 위쪽 방향을 길게 눌러 맞춘 뒤 결정하세요.
- 이름을 정하는 단계에서 Windows 입력 상자가 열립니다. 이름을 쓰고 Enter를 누르세요.
- 첫 주행은 브랜즈 해치의 트랙 데이입니다. 끝까지 달리면 메인 화면(My Home)이 나오고 세이브가 만들어집니다.

런처에는 소니의 키가 없습니다. 설치하는 동안 RPCS3가 게임의 실행 파일과 시스템 모듈 하나를 복호화하며, 그 과정의 어떤 것도 남기지 않고, 모든 결과를 포트를 만든 원본 파일의 해시와 맞춰 봅니다.

### 모드

**GT6 Career Mod (+ Spec II mod)** — 만든 사람 Lunard, Spec II Beta 7(Nenkai, TheAdmiester)을 바탕으로 Strato mod(Emivebe)와 Performance mod(Plunke)의 기능을 담았습니다.

1. 게임을 한 번 시작해 메인 화면(My Home)까지 가서 세이브를 만든 뒤 게임을 끕니다.
2. [GTPlanet 페이지](https://www.gtplanet.net/forum/downloads/gran-turismo-6-career-mod.7049/)에서 "GT6 Career mod ver. ….zip"을 받습니다.
3. 런처: **모드 → 압축 파일 가져오기…**로 그 zip을 고릅니다. Spec II와 커리어 모드가 올바른 순서로 켜집니다.

커리어 모드를 처음 쓸 때는 새 세이브로 시작하세요. 이 프로젝트는 다른 사람의 모드를 받거나, 함께 넣거나, 다시 올리지 않습니다.

### 업데이트

런처는 하루에 한 번 이 저장소의 릴리스를 확인합니다. 새 릴리스가 있으면 뒤에서 받아 두었다가 다음에 런처를 열 때 적용합니다. 게임 파일, 세이브, 설정, 모드는 그대로 남습니다.

### 크레딧

- **[RPCS3](https://github.com/RPCS3/rpcs3)**: PlayStation 3 에뮬레이터입니다. 런처는 설치하는 동안 RPCS3 공식 빌드로 게임의 실행 파일과 펌웨어 모듈 하나를 복호화하고, 끝나면 지웁니다. 빌드는 RPCS3의 GT6 1.22용 커뮤니티 패치(AlexHaust, illusion 외)를 포트의 스위치로 만들며, PS3가 그란 투리스모 6를 어떻게 돌리는지에 대한 RPCS3의 작업이 이 포트의 기준이 되었습니다.
- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)**(sp00nznet): 이 포트의 바탕인 정적 리컴파일러와 런타임입니다.
- **[MABEC](https://github.com/Mabec-games/Gran-Turismo-6-PC)**(Gran Turismo 6 PC, RPCS3용 GT6 빌드): PSN 없는 LAN 플레이 - 숨은 부팅 인자 "lanmode"와 abowen의 "LAN Room Sync Without PSN" 패치, RPCS3의 "Develop Settings" - 가 포트 LAN 모드의 바탕이고, Edo의 "Looser Chase Camera" 패치(포트의 느슨한 추적 카메라)와 게임 자체의 크레딧 설정 방법도 그 기록에서 알았습니다. 그 코드는 쓰지 않았고 사실만 썼습니다. 감사합니다.
- **[Dear ImGui](https://github.com/ocornut/imgui)**: 게임 안 메뉴입니다.

### 빌드(개요)와 라이선스

빌드 방법은 [BUILD.md](BUILD.md)(영어)에 있고, 라이선스는 위의 영어 부분(License)과 같습니다. 포트 자체의 코드는 [MIT 라이선스](LICENSE)이고, 게임과 게임에서 나온 것(릴리스 exe 안의 리컴파일된 게임 코드 포함)은 라이선스 대상이 아니며 Sony Interactive Entertainment와 Polyphony Digital의 것입니다.
