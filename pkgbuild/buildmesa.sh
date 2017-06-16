#!/bin/bash

# Settings
flavor=""  # "amd-only" or ""
fragment="branch=master" #  or "tag=" or "commit=" or "branch="



# either empty ("") or amd-only
# amd-only build for both amdgpu and radeon kernel driver
# empty is suitable for amd and intel igd

pckflavor=""
[ -z ${flavor} ]  || pckflavor="${flavor}."


function die {
  message=$1
  echo $message 
  exit -1
}

function check_available {
  program=$1
  which $program &> /dev/null 
}

function check_mesa_git_configured {
  cat /etc/pacman.conf  |grep -E '(\[.*\])' |grep -v "#" |head -2 |tail -1
}

function check_multilib_configured {
  cat /etc/pacman.conf  |grep -E '(\[.*\])' |grep -v "#" |grep -q multilib
}
function install_packages {
  pre_packages="llvm-svn llvm-ocaml-svn llvm-libs-svn  libclc-git  lib32-llvm-svn lib32-llvm-libs-svn clang-tools-extra-svn clang-svn"
  sudo pacman -Syy
  sudo pacman -Sdd $pre_packages  --noconfirm --needed --force
}

# pre_check

[ $UID -eq 0 ] &&  die "This script must be ran by and normal user with sudo configured"

[ "x[mesa-git]" == "x$(check_mesa_git_configured)" ] || die "Mesa git must be in first position in /etc/pacman.conf"
check_multilib_configured || die "Multilib must be configured in /etc/pacman.conf"


# program check

echo "Checking availability of various tools"

for program in pacman makepkg sudo git gcc; do 
  echo -n "checking  $program :"
  check_available $program  || die "program $program must be installed";
  echo "ok"
done

GIT_REPOSITORIES=(  "git://anongit.freedesktop.org/drm/libdrm"  "https://github.com/mikakev1/mesa_mild_compatibility.git"  )
GIT_NAMES=(  "libdrm"  "mesa"  )

echo "installing  packages"
install_packages

echo "Starting to clone repositories"

for (( i=0; i < ${#GIT_REPOSITORIES[@]}; i++)); do 
echo Cloning ${GIT_REPOSITORIES[$i]}
git clone ${GIT_REPOSITORIES[$i]}  ${GIT_NAMES[$i]}
done

echo "Retrieving PKGBUILD for libdrm and mesa"
mkdir -p PKGBUILD/{mesa64,mesa32,libdrm64,libdrm32}

curdir=$(pwd)
mesasource="${curdir}/mesa"
drmsource="${curdir}/libdrm"
pkgbuilddir="$mesasource/pkgbuild"


PKGBUILD_NAME=( "PKGBUILD.${pckflavor}libdrm64" "PKGBUILD.${pckflavor}libdrm32" "PKGBUILD.${pckflavor}mesa64" "PKGBUILD.${pckflavor}mesa32" )
PKGBUILD_DEST=( PKGBUILD/libdrm64 PKGBUILD/libdrm32 PKGBUILD/mesa64 PKGBUILD/mesa32 )
PKGBUILD_URL=( "git+file://${drmsource}" "git+file://${drmsource}" "git+file://${mesasource}#${fragment}"  "git+file://${mesasource}#${fragment}" )

for (( i=0; i < ${#PKGBUILD_DEST[@]}; i++ )); do
  echo "copying  $pkgbuilddir/${PKGBUILD_NAME[$i]} to ${PKGBUILD_DEST[$i]}/PKGBUILD"
  cp  "$pkgbuilddir/${PKGBUILD_NAME[$i]}" "${PKGBUILD_DEST[$i]}/PKGBUILD.unmodified"
  cat  "${PKGBUILD_DEST[$i]}/PKGBUILD.unmodified" | awk -v source="${PKGBUILD_URL[$i]}" '{if ($1 ~ /source=/) { printf ("source=( \"%s\" )\n",source) } else { print }}' > ${PKGBUILD_DEST[$i]}/PKGBUILD
done

#SETTING ENVIRONMENT

for directory in  ${PKGBUILD_DEST[*]}; do
  echo "building in $directory"
  cd $directory
  makepkg -sCcf
  cd ..;cd ..
done


echo "* NOTICE *: if system is unbootable after installation boot kernel with commandline < modprobe.blacklist=amdgpu,radeon > and reinstall normal packages from mesa-git"
echo "To install run "
find ./ -name "*xz" -exec echo "sudo pacman -Udd {} --force" \; | sort


