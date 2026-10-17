# Deskflow com área de transferência no Hyprland

> **Este é um fork pessoal.** O branch `hyprland` é o Deskflow **1.27.0 oficial** (tag `v1.27.0`) com três
> commits de código e este texto por cima. Para ver exatamente o que muda: `git log v1.27.0..hyprland`.
> O README original do projeto continua logo abaixo desta seção.

## Por que este fork existe

O cenário: servidor Linux com **Hyprland** (Wayland) controlando um cliente **Windows 11**.

Com o Deskflow 1.27 oficial, mouse e teclado funcionavam, mas **copiar e colar não funcionava em nenhum sentido**:

- No Wayland, a 1.27 só compartilha a área de transferência pelo portal XDG (`org.freedesktop.portal.Clipboard`).
  O portal do Hyprland (`xdg-desktop-portal-hyprland` 1.4.1, a versão mais recente em outubro de 2026) não
  implementa essa interface.
- Até a 1.26 havia um caminho alternativo (`wlClipboard`), que chamava `wl-copy` e `wl-paste` como programas
  externos. Ele foi removido na 1.27, e os mantenedores disseram que não vão trazê-lo de volta
  ([deskflow/deskflow#10135](https://github.com/deskflow/deskflow/issues/10135)).
- Esse caminho antigo também causava problemas sérios, observados aqui na 1.26:
  - o `wl-copy` herdava o socket do servidor e continuava segurando a porta 24800 depois que o servidor parava.
    O cliente conectava numa porta que ninguém atendia, e o servidor reiniciado não conseguia abrir a porta;
  - a cada segundo ele lia a área de transferência inteira, imagem incluída;
  - podia bloquear o servidor por até 30 s esperando um `wl-paste`.

## O que muda

### 1. Área de transferência pelo `ext-data-control` (`feat(wayland)`)

O Hyprland oferece o protocolo `ext-data-control-v1`, padronizado no pacote oficial `wayland-protocols`. É o
mesmo canal que os gerenciadores de área de transferência usam, e o que o `wl-copy` usa por baixo. O servidor
passa a falar com ele diretamente:

- **Só entra quando o portal não tem área de transferência.** Com portal (KDE, GNOME), o comportamento oficial
  não muda. Se um dia o Hyprland implementar o portal, este código se desliga sozinho.
- Conexão Wayland própria, numa thread separada: nada bloqueia o servidor.
- Nenhum processo externo, e todos os descritores com `CLOEXEC`.
- Leitura e escrita com prazo máximo e limite de tamanho.
- A própria seleção leva o tipo `application/x-deskflow-owned`, para não haver eco.
- Texto em UTF-8. Imagens em PNG no Linux e em DIB na rede do Deskflow, com a conversão que a 1.27 já tem.

Arquivos: `src/lib/platform/WlDataControlClipboard.{h,cpp}` (novo) e as ligações em `EiComputer`,
`PortalInputCapture` e `src/lib/platform/CMakeLists.txt`.

### 2. Não mandar área de transferência vazia na troca de tela (`fix(server)`)

Cada cliente começa com todas as áreas de transferência marcadas como pendentes. Na primeira troca de tela
depois que um cliente conecta, o servidor mandava a área principal, com o conteúdo, e logo depois a área de
seleção, vazia. O cliente Windows grava as duas no mesmo lugar e ficava com a área de transferência vazia, a cada
reconexão. Agora o servidor não manda área vazia; as demais trocas continuam iguais.

Arquivo: `src/lib/server/Server.cpp`.

### 3. Rolagem do mouse no Hyprland (`fix(wayland)`)

O Hyprland 0.56 manda cada clique da roda duas vezes na captura: como rolagem contínua (15) e como rolagem por
clique, que deveria valer 120 mas leva o mesmo 15. A 1.27 somava as duas num único acumulador, uma contada em
cliques e a outra em unidades de 120, e a partir do segundo clique da roda o Windows recebia uns 16 cliques de
uma vez (cerca de 50 linhas). Agora o servidor:

- descarta a cópia por clique quando a rolagem contínua já veio no mesmo quadro;
- converte 15 de rolagem contínua em 1 clique, a escala do próprio Hyprland e do libinput;
- guarda os restos das duas rolagens separados, para que uma não infle a outra.

Cada clique da roda chega ao cliente como exatamente 1 clique (120 unidades), como um mouse ligado nele.

Arquivo: `src/lib/platform/EiComputer.cpp`.

## Como foi validado (16/10/2026)

Teste automático contra o Hyprland real, com `wl-copy` e `wl-paste` do outro lado:

| Caso | Resultado |
| --- | --- |
| Texto com acentos e emoji, Linux → Deskflow | idêntico |
| Texto, Deskflow → Linux | idêntico, sem eco |
| Imagem PNG 3840x2160, Linux → Deskflow | pixels idênticos |
| Imagem 1920x1080, Deskflow → Linux | pixels idênticos, sem eco |
| Descritores e processos | estáveis, nenhum processo filho |

Ponta a ponta, servidor Hyprland com cliente Windows 11 no Deskflow 1.27.0 oficial:

| Sentido | Resultado |
| --- | --- |
| Texto Linux → Windows | idêntico, byte a byte (acentos, emoji, japonês) |
| Texto Windows → Linux | idêntico, byte a byte (acentos, emoji, chinês) |
| Imagem Linux → Windows (1920x1080) | 0 pixel diferente |
| Imagem Windows → Linux (1280x720) | 0 pixel diferente |

Em 16 trocas de tela, nenhum travamento do servidor e nenhum processo pendurado.

Rolagem (17/10/2026), gravada no Windows com um gancho de mouse de baixo nível enquanto a roda girava para os
dois lados: dos 175 eventos que chegaram, todos valiam exatamente ±120, na vertical e na horizontal. Antes da
correção, cada clique da roda chegava como ±1920 ou ±2040.

## Limitação conhecida

Se um programa no Windows copiar algo **enquanto o cursor está no Linux**, a cópia não chega. Nesse momento o
cliente Windows manda a área principal vazia e o conteúdo só na área de seleção, que o servidor no Wayland
ignora. No uso normal, copiando com o cursor no Windows, o conteúdo vai na saída da tela.

## Compilar e instalar (Arch Linux)

Por que compilar: o pacote Arch publicado na release 1.27.0 foi gerado com uma `libportal` mais nova que a 0.10 do
Arch e não abre (`undefined symbol: xdp_global_shortcut_free`). O código-fonte da 1.27 compila com a 0.10; só os
atalhos globais do portal ficam desligados.

Além das dependências do Deskflow, são necessários `wayland` (cliente e `wayland-scanner`) e
`wayland-protocols` 1.39 ou mais novo.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/opt/deskflow" -DBUILD_TESTS=OFF
cmake --build build -j 16
cmake --install build
```

A configuração deve mostrar `wayland data-control clipboard: ON`. Ao subir o servidor, o log mostra
`clipboard: using wayland data-control (ext-data-control-v1)`.

## Atualizar para uma versão nova do Deskflow

```sh
git fetch origin --tags
git rebase --onto v1.28.0 v1.27.0 hyprland   # troque v1.28.0 pela versão nova
cmake --build build -j 16 && cmake --install build
# refazer o teste de copiar e colar nos quatro sentidos e o de rolagem antes de publicar
git push --force-with-lease cidico hyprland
```

Este fork deixa de ser necessário quando o Hyprland implementar o portal de área de transferência ou quando o
Deskflow aceitar um caminho equivalente.

## Ajustes de ambiente feitos junto (fora do código)

- **Teclado do Windows trocando sozinho para ABNT.** Aqui o Windows usa só "Estados Unidos (internacional)", mas
  os perfis do sistema (tela de boas-vindas e a conta SYSTEM, que roda o serviço do Deskflow) tinham o ABNT como
  segundo teclado. O serviço trazia esse ABNT para a sessão como layout fantasma, e o atalho Alt+Shift alternava
  para ele, quebrando acentos e ç. A correção foi copiar as configurações do usuário para o sistema
  (`Copy-UserInternationalSettingsToSystem -WelcomeScreen $true -NewUser $true`), desligar os atalhos de troca de
  teclado e descarregar o ABNT fantasma. Para conferir, o log do cliente deve listar um só layout
  (`local layouts: pt`).
- **Aviso "Missing Keyboard Layouts" no Windows.** Ele compara idiomas, não teclados: o EUA internacional do Linux
  é anunciado como `en`, e o do Windows como `pt`. O aviso só importa para a sincronização de idioma, que vem
  desligada, e foi silenciado com `gui/ignoreMissingKeyboardLayouts=true`.

---

<!-- README original do Deskflow a partir daqui -->

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://github.com/deskflow/deskflow-artwork/blob/main/logo/deskflow-logo-dark-200px.png?raw=true">
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/deskflow/deskflow-artwork/blob/main/logo/deskflow-logo-light-200px.png?raw=true">
  <img alt="Deskflow" src="https://github.com/user-attachments/assets/f005b958-24df-4f4a-9bfd-4f834dae59d6">
</picture>

**Deskflow** is a free and open source keyboard and mouse sharing app.
Use the keyboard, mouse, or trackpad of one computer to control nearby computers,
and work seamlessly between them.
It's like a software KVM (but without the video).
TLS encryption is enabled by default. Wayland is supported. Clipboard sharing is supported.

> [!TIP]
>
> **Chat with us**
>
> - Main discussion on Matrix: [`#deskflow:matrix.org`](https://matrix.to/#/#deskflow:matrix.org) ([Matrix clients](https://matrix.org/ecosystem/clients/))
> - Discussion also happens on IRC: `#deskflow` or `#deskflow-dev` on [Libera Chat](https://libera.chat/)
> - Start a [new discussion](https://github.com/deskflow/deskflow/discussions) on our GitHub project.

## Download

[![Downloads: Stable Release](https://img.shields.io/github/downloads/deskflow/deskflow/latest/total?style=for-the-badge&logo=github&label=Download%20Stable)](https://github.com/deskflow/deskflow/releases/latest)&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;[![Downloads: Continuous Build](https://img.shields.io/github/downloads/deskflow/deskflow/continuous/total?style=for-the-badge&logo=github&label=Download%20Continuous)](https://github.com/deskflow/deskflow/releases/continuous)&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;[![Download From Flathub](https://img.shields.io/flathub/downloads/org.deskflow.deskflow?style=for-the-badge&logo=flathub&label=Download%20from%20flathub)](https://flathub.org/apps/org.deskflow.deskflow)

> [!NOTE]
> On Windows, you will need to install the
> [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-microsoft-visual-c-redistributable-version).  
> Download latest: [`vc_redist.x64.exe`](https://aka.ms/vc14/vc_redist.x64.exe) [`vc_redist.arm64.exe`](https://aka.ms/vc14/vc_redist.arm64.exe)

> [!TIP]
> For macOS users, the easiest way to install and stay up to date is to use [Homebrew](https://brew.sh) with our [homebrew-tap](https://github.com/deskflow/homebrew-tap).
> macOS reports unsigned apps as damaged. This occurs because we do not use an Apple certificate for notarization. Clear the quarantine attribute to run the app: `xattr -c Deskflow.app`

To use Deskflow, download one of our [packages](https://github.com/deskflow/deskflow/releases), install `deskflow` (from your package repository), or [build it](https://github.com/deskflow/deskflow/wiki/Building) from source.

## Stats

[![GitHub commit activity](https://img.shields.io/github/commit-activity/m/deskflow/deskflow?logo=github)](https://github.com/deskflow/deskflow/commits/master/)
[![GitHub top language](https://img.shields.io/github/languages/top/deskflow/deskflow?logo=github)](https://github.com/deskflow/deskflow/commits/master/)
[![GitHub License](https://img.shields.io/github/license/deskflow/deskflow?logo=github)](LICENSE)
[![REUSE status](https://api.reuse.software/badge/github.com/deskflow/deskflow)](https://api.reuse.software/info/github.com/deskflow/deskflow)

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=deskflow_deskflow&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=deskflow_deskflow)
[![Coverage](https://sonarcloud.io/api/project_badges/measure?project=deskflow_deskflow&metric=coverage)](https://sonarcloud.io/summary/new_code?id=deskflow_deskflow)
[![Code Smells](https://sonarcloud.io/api/project_badges/measure?project=deskflow_deskflow&metric=code_smells)](https://sonarcloud.io/summary/new_code?id=deskflow_deskflow)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=deskflow_deskflow&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=deskflow_deskflow)

[![CI](https://github.com/deskflow/deskflow/actions/workflows/continuous-integration.yml/badge.svg)](https://github.com/deskflow/deskflow/actions/workflows/continuous-integration.yml)
[![CodeQL Analysis](https://github.com/deskflow/deskflow/actions/workflows/codeql-analysis.yml/badge.svg)](https://github.com/deskflow/deskflow/actions/workflows/codeql-analysis.yml)
[![SonarCloud Analysis](https://github.com/deskflow/deskflow/actions/workflows/sonarcloud-analysis.yml/badge.svg)](https://github.com/deskflow/deskflow/actions/workflows/sonarcloud-analysis.yml)

## Contribute

[![Good first issues](https://img.shields.io/github/issues/deskflow/deskflow/good%20first%20issue?label=good%20first%20issues&color=%2344cc11)](https://github.com/deskflow/deskflow/labels/good%20first%20issue)

There are many ways to contribute to the Deskflow project.

We're a friendly, active, and welcoming community focused on building a great app.

Read our [Contributing](https://github.com/deskflow/deskflow/wiki/Contributing) page to get started.

For instructions on building Deskflow, use the wiki page: [Building](https://github.com/deskflow/deskflow/wiki/Building)

## Operating Systems

We support all major operating systems, including Windows, macOS, Linux, and Unix-like BSD-derived.

Windows 10 v1809 or higher is required.

macOS 13 or higher is required to use our CI builds for Apple Silicon machines. macOS 12 or higher is required for Intel macs or local builds.

Linux requires libei 1.3+ and libportal 0.8+ for the server/client. Additionally, Qt 6.7+ is required for the GUI.
Linux users with systems not meeting these requirements should use flatpak in place of a native package.

We officially support FreeBSD, and would also like to support: OpenBSD, NetBSD, DragonFly, Solaris.

## Repology

Repology monitors a huge number of package repositories and other sources comparing package
versions across them and gathering other information.

[![Repology](https://repology.org/badge/vertical-allrepos/deskflow.svg?columns=2&exclude_unsupported)](https://repology.org/project/deskflow/versions)

## Installing on macOS

When you install Deskflow on macOS, you need to allow accessibility access (Privacy & Security) to both the `Deskflow` app and the `deskflow` process.

If using Sequoia, you may also need to allow `Deskflow` under Local Network‍ settings (Privacy & Security).
When prompted by the OS, go to the settings and enable the access.

If you are upgrading and you already have `Deskflow` or `deskflow`
on the allowed list you will need to manually remove them before accessibility access can be granted to the new version.

macOS users who download directly from releases may need to run `xattr -c /Applications/Deskflow.app` after copying the app to the `Applications` dir.

It is recommended to install Deskflow using [Homebrew](https://brew.sh) from our [homebrew-tap](https://github.com/deskflow/homebrew-tap)

To add our tap, run:

```
brew tap deskflow/tap
```

Then install either:

- Stable: `brew install deskflow`
- Continuous: `brew install deskflow-dev`

## Similar Projects

In the open source developer community, similar projects collaborate for the improvement of all
mouse and keyboard sharing tools. We aim for idea sharing and interoperability.

- [**Lan Mouse**](https://github.com/feschber/lan-mouse) -
  Rust implementation with the goal of having native front-ends and interoperability with
  Deskflow/Synergy.
- [**Synergy**](https://symless.com/synergy) -
  Downstream commercial fork. Synergy sponsors Deskflow with financial support and contributes code ([learn more](https://github.com/deskflow/deskflow/wiki/Relationship-with-Synergy)).
- [**Input Leap**](https://github.com/input-leap/input-leap) -
  Inactive Deskflow/Synergy-derivative with the goal continuing Barrier development (now a dead fork).

## FAQ

### Is Deskflow compatible with Synergy, Input Leap, or Barrier?

Yes, Deskflow has network compatibility with all forks:

- Requires Deskflow >= v1.17.0.96
- Deskflow will _just work_ with Input Leap and Barrier (server or client).
- Connecting a Deskflow client to a Synergy 1 server will also _just work_.
- To connect a Synergy 1 client, you need to select the Synergy protocol in the Deskflow server settings.

_Note:_ Only Synergy 1 is compatible with Deskflow (Synergy 3 is not yet compatible).

### Is Deskflow compatible with Lan Mouse?

We would love to see compatibility with Lan Mouse. This may be quite an effort as currently the way they handle the generated input is very different.

### If I want to solve issues in Deskflow do I need to contribute to a fork?

We welcome PRs (pull requests) from the community. If you'd like to make a change, please feel
free to [start a discussion](https://github.com/deskflow/deskflow/discussions) or
[open a PR](https://github.com/deskflow/deskflow/wiki/Contributing).

### Is clipboard sharing supported?

Absolutely. The clipboard-sharing feature is a cornerstone feature of the product and we are
committed to maintaining and improving that feature.

### Is Wayland for Linux supported?

Yes! Wayland (the Linux display server protocol aimed to become the successor of the X Window
System) is an important platform for us.
The [`libei`](https://gitlab.freedesktop.org/libinput/libei) and
[`libportal`](https://github.com/flatpak/libportal) libraries enable
Wayland support for Deskflow. We would like to give special thanks to Peter Hutterer,
who is the author of `libei`, a major contributor to `libportal`, and the author of the Wayland
implementation in Deskflow. Others such as Olivier Fourdan and Povilas Kanapickas helped with the
Wayland implementation.

Some features _may_ be unavailable or broken on Wayland. Please see the [known Wayland issues](https://github.com/deskflow/deskflow/discussions/7499).

### Where did it all start?

Deskflow was first created as Synergy in 2001 by Chris Schoeneman.
Read about the [history of the project](https://github.com/deskflow/deskflow/wiki/History) on our
wiki.

## Meow'Dib (our mascot)

![Meow'Dib](https://github.com/user-attachments/assets/726f695c-3dfb-4abd-875d-ed658f6c610f)

## Deskflow Contributors

[![Sponsored by Synergy](https://raw.githubusercontent.com/deskflow/deskflow-artwork/b2c72a3e60a42dee793bd47efc275b5ee0bdaa5f/misc/synergy-sponsor.svg)](https://symless.com/synergy)

[Synergy](https://symless.com/synergy) sponsors the Deskflow project by contributing code and providing financial support ([learn more](https://github.com/deskflow/deskflow/wiki/Relationship-with-Synergy)).

Deskflow is made by possible by these contributors.

 <a href = "https://github.com/deskflow/deskflow/graphs/contributors">
   <img src = "https://contrib.rocks/image?repo=deskflow/deskflow"/>
 </a>

## License

This project is licensed under [GPL-2.0](LICENSE) with an [OpenSSL exception](../LICENSES/LicenseRef-OpenSSL-Exception.txt).
