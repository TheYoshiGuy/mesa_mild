#!/bin/bash
# BUG WORKED AROUND: using -march=native -O2 breaks steam https://bugs.freedesktop.org/show_bug.cgi?id=101484

# adding revert git revert 2b8b9a56efc24cc0f27469bf1532c288cdca2076
# deprecated #NOTICE:WARNING building mesa with -O2 and native optimisations breaks steam with Haswell+
#NOTICE:INFO Changing RENDERER is sometimes required for intel IGD
#NOTICE:INFO Ensure MAKEFLAGS is correctly set up in buildpkg (unless you're fond of wasting time)
#NOTICE:ATTENTION Please, setup (or check it's set) << \\n [mesa-git] \\n Server = http://pkgbuild.com/~lcarlier/$repo/$arch \\n >>  as first entry after [config]'s section in /etc/pacman.conf
#NOTICE:INFO You must edit setting called "flavor" in this script if you want to remove intel IGPU support
#NOTICE:WARNING If installation fails because of dependencies, you will need to force (using sudo pacman -Udd) mesa_mild installation

# Settings
flavor=""  # "amd-only" or "" for intel's IGD
fragment="branch=master" #  or "tag=" or "commit=" or "branch="
declare -A SRC_REPOSITORIES=(  [mesa]="https://github.com/mikakev1/mesa_mild_compatibility.git" [libdrm]="git://anongit.freedesktop.org/drm/libdrm" )

# GLOBALS
curdir=$(pwd)

if [ "x${flavor}" == "x" ]; then
pckflavor=""
else
pckflavor="${flavor}."
fi


mesasource="${curdir}/mesa"
drmsource="${curdir}/libdrm"
pkgbuilddir="$mesasource/pkgbuild"


# Functions
function ask_clean {
[ -d "${curdir}/mesa" ]  && Log "WARNING" "Please remove ${curdir}/mesa unless you know why you need it"
[ -d "${curdir}/libdrm" ]  && Log "WARNING" "Please remove ${curdir}/libdrm  unless you know why you need it"
}


function pre_clean {
[ -d "${curdir}/PKGBUILD" ]  && rm -rf ${curdir}/PKGBUILD
}



function Log {
declare -A ColorStatus=( [CRITICAL]='\e[31m' [reset]='\e[39m' [NORMAL]='\e[39m' [OK]='\e[32m' [ATTENTION]='\e[31m'   [WARNING]='\e[33m' [INFO]='\e[36m'  )
  STATUS=$1;
   shift

    tput sgr0
    echo -e   ${ColorStatus[${STATUS}]}$STATUS    $@
    tput sgr0
}

function display_notices {
    myname=$0
    Log INFO "Acknowledge this before continuing"
    cat $myname |egrep '^#NOTICE:' |sed s/\#NOTICE:// |while read line ;do Log $line;done
    echo "Press ENTER to continue or CTRL+C to interrupt"
    read
}

function die {
  Log "CRITICAL" $message
  exit -1
}

function check_available {
  program=$1
  which $program &> /dev/null
}

function program_check {
    Log "INFO" "Checking availability of required binaries"

    for program in pacman makepkg sudo gcc; do
        echo -n " > checking  $program :"
        check_available $program  || die "program $program must be installed";
        echo "ok"
    done
}

function check_mesa_git_configured {
  cat /etc/pacman.conf  |grep -E '(\[.*\])' |grep -v "#" |head -2 |tail -1
}

function check_multilib_configured {
  cat /etc/pacman.conf  |grep -E '(\[.*\])' |grep -v "#" |grep -q multilib
}

function install_packages {
  Log "INFO" "Installing / updating packages"
  pre_packages="llvm-svn llvm-ocaml-svn llvm-libs-svn  libclc-git  lib32-llvm-svn lib32-llvm-libs-svn clang-tools-extra-svn clang-svn libunwind lib32-libunwind git"
  sudo pacman -Syy
  sudo pacman -Sdd $pre_packages  --noconfirm --needed --force
}

function clone_repositories {
Log "INFO" "Cloning repositories"

for name in ${!SRC_REPOSITORIES[*]} ; do
        Log "INFO" "Cloning $name from  ${SRC_REPOSITORIES[${name}]}"
        git clone ${SRC_REPOSITORIES[${name}]} $name  &> /dev/null
done
}

function pre_check {
    [ $UID -eq 0 ] && die "This script must be ran by and normal user with sudo configured"
    [ "x[mesa-git]" == "x$(check_mesa_git_configured)" ] || die "Mesa git must be in first position in /etc/pacman.conf"
    check_multilib_configured || die "Multilib must be configured in /etc/pacman.conf"
}

function prebuild_all {
    Log "INFO" "Retrieving PKGBUILD for libdrm and mesa"
    mkdir -p PKGBUILD/{mesa64,mesa32,libdrm64,libdrm32}
    PKGBUILD_NAME=( "PKGBUILD.${pckflavor}libdrm64" "PKGBUILD.${pckflavor}libdrm32" "PKGBUILD.${pckflavor}mesa64" "PKGBUILD.${pckflavor}mesa32" )
    PKGBUILD_LIBDRMDEST=( PKGBUILD/libdrm64 PKGBUILD/libdrm32 )
    PKGBUILD_DEST=( PKGBUILD/libdrm64 PKGBUILD/libdrm32 PKGBUILD/mesa64 PKGBUILD/mesa32 )
    PKGBUILD_MESADEST=( PKGBUILD/mesa64 PKGBUILD/mesa32 )
    PKGBUILD_URL=( "git+file://${drmsource}" "git+file://${drmsource}" "git+file://${mesasource}#${fragment}"  "git+file://${mesasource}#${fragment}" )

    for (( i=0; i < ${#PKGBUILD_DEST[@]}; i++ )); do
        echo "copying  $pkgbuilddir/${PKGBUILD_NAME[$i]} to ${PKGBUILD_DEST[$i]}/PKGBUILD"
        cp  "$pkgbuilddir/${PKGBUILD_NAME[$i]}" "${PKGBUILD_DEST[$i]}/PKGBUILD.unmodified"
        cat  "${PKGBUILD_DEST[$i]}/PKGBUILD.unmodified" | awk -v source="${PKGBUILD_URL[$i]}" '{if ($1 ~ /source=/) { printf ("source=( \"%s\" )\n",source) } else { print }}' > ${PKGBUILD_DEST[$i]}/PKGBUILD
    done
}

function build_and_install_libdrm {
    for directory in  ${PKGBUILD_LIBDRMDEST[*]}; do
        echo "building in $directory"
        cd $directory
        makepkg -siCcf
        cd ..;cd ..
    done
}

function build_and_install_mesa {
    for directory in  ${PKGBUILD_MESADEST[*]}; do
        echo "building in $directory"
        cd $directory
        makepkg -sCcf
        sudo find .  -name "*.xz" -exec pacman -Udd {} --force \;
        cd ..;cd ..
    done
}

function end_notice {
    Log "WARNING" "if system is unbootable after installation boot kernel with commandline < modprobe.blacklist=amdgpu,radeon > and reinstall normal packages from mesa-git"
    Log "INFO" "Remember to add 'IgnoreGroup = mesagit' to /etc/pacman.conf if you're satisfied (or take the risk to have it all overwritten)"
}
function mesa_revert {
  Log "INFO" "Reverting 2b8b9a56efc24cc0f27469bf1532c288cdca2076"
  cd mesa
  git revert 2b8b9a56efc24cc0f27469bf1532c288cdca2076 --no-edit
  cd ..
}

#MAIN
trap "die 'Interrupted by user'" SIGHUP SIGINT SIGTERM
pre_clean
display_notices
pre_check
program_check
install_packages
clone_repositories
mesa_revert
prebuild_all
build_and_install_libdrm
build_and_install_mesa

end_notice
ask_clean



