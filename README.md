Microsoft Git
===============

[![CI/PR](https://github.com/microsoft/git/actions/workflows/main.yml/badge.svg)](https://github.com/microsoft/git/actions/workflows/main.yml)

This is Microsoft Git, a special Git distribution to support monorepo scenarios. If you are _not_ working in a monorepo, you are likely searching for [Git for Windows](http://git-for-windows.github.io/) instead of this codebase.

If you encounter problems with Microsoft Git, please report them as [GitHub issues](https://github.com/microsoft/git/issues).

Why is Microsoft Git needed?
=========================================================

Git is awesome - it's a fast, scalable, distributed version control system with an unusually rich command set that provides both high-level operations and full access to internals. What more could you ask for?

Well, because Git is a distributed version control system, each Git repository has a copy of all files in the entire history. As large repositories, aka _monorepos_ grow, Git can struggle to manage all that data. As Git commands like `status` and `fetch` get slower, developers stop waiting and start switching context. And context switches harm developer productivity.

Microsoft Git is focused on addressing these performance woes and making the monorepo developer experience first-class. It does so in part by working with the [GVFS protocol](https://docs.microsoft.com/en-us/azure/devops/learn/git/gvfs-architecture#gvfs-protocol) to prefetch packs of commits and trees and delay downloading of associated blobs. This is required for monorepos using [VFS for Git](https://github.com/microsoft/VFSForGit/blob/master/Readme.md). Additionally, some Git hosting providers support the GVFS protocol instead of the Git-native [partial clone feature](https://github.blog/2020-12-21-get-up-to-speed-with-partial-clone-and-shallow-clone/).

Downloading and Installing
=========================================================

If you're working in a monorepo and want to take advantage of Microsoft Git's performance boosts, you can
download the latest version installer for your OS from the [Releases page](https://github.com/microsoft/git/releases). Alternatively,
you can opt to install via the command line, using the below instructions for supported OSes:

## Windows
__Note:__ Winget is still in public preview, meaning you currently [need to take special installation steps](https://docs.microsoft.com/en-us/windows/package-manager/winget/#install-winget) (i.e. manually installing the `.appxbundle`, installing the preview version of [App Installer](https://www.microsoft.com/p/app-installer/9nblggh4nns1?ocid=9nblggh4nns1_ORSEARCH_Bing&rtc=1&activetab=pivot:overviewtab), or participating in the [Windows Insider flight ring](https://insider.windows.com/https://insider.windows.com/)).

To install with Winget, run

```shell
winget install microsoft/git
```

To upgrade Microsoft Git, use the following Git command, which will download and install the latest release.

```shell
git update-microsoft-git
```

You may also be alerted with a notification to upgrade, which presents a single-click process for running `git update-microsoft-git`.

## macOS

To install Microsoft Git on macOS, first [be sure that Homebrew is installed](https://brew.sh/) then install the `microsoft-git` cask with these steps:

```shell
brew tap microsoft/git
brew install --cask microsoft-git
```

To upgrade microsoft/git, you can run the necessary brew commands:

```shell
brew update
brew upgrade --cask microsoft-git
```

Or you can run the `git update-microsoft-git` command, which will run those brew commands for you.

## Linux

For Ubuntu/Debian distributions, `apt-get` support is coming soon. For now, though, please use the most recent [`.deb` package](https://github.com/microsoft/git/releases).

```shell
wget -o microsoft-git.deb https://github.com/microsoft/git/releases/download/v2.31.1.vfs.0.1/git-vfs_2.31.1.vfs.0.1.deb
sudo dpkg -i microsoft-git.deb
```

For other distributions, you will need to compile and install microsoft/git from source:

```shell
git clone https://github.com/microsoft/git microsoft-git
cd microsoft-git
make -j12 prefix=/usr/local
sudo make -j12 prefix=/usr/local install
```

For more assistance building Git from source, see [the INSTALL file in the core Git project](https://github.com/git/git/blob/master/INSTALL).

Contributing
=========================================================

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit <https://cla.microsoft.com.>

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
