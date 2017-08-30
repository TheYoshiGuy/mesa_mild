This version incorporate some modifications  for GALLIUM3D drivers and i965:

- Include a new switch in driconf "Allow a relaxed core profile"
- Include DSA in compatibility profile
- Include a way to override VENDOR and RENDERER via environment


Note: If you enable "Allow a relaxed core profile", you must disable glthread  in driconf

The modifications shall be used only in conjunction with

 - MESA_GL_VERSION_OVERRIDE=4.5 for "Allow a relaxed core profile"

 - MESA_GL_VERSION_OVERRIDE=4.5COMPAT and "Allow an higher compat profile for application that request it"

Some applications will require additionaly "Force GLSL extension behavior to 'warn'"

Cemu specifics :

 - In the binary Cemu.exe, search & replace "#version 420" by "#version 450"
 - Enable "Force GLSL extension behavior to 'warn'" in driconf
 - If you're brave enough to use an intel IGD, you must set

  MESA_VENDOR_OVERRIDE="mesa" MESA_RENDERER_OVERRIDE="mesa"


  (Of course, unless you want to endup letter box display)


In all cases, use  :

MESA_GL_VERSION_OVERRIDE=4.5COMPAT wine Cemu.exe

Others applications :


Somme applications need a compatibility profile, this fork can help, BUT
Be sure to check at first (to cope with the burden of building this) if mesa is working, for this check perticulary :
- That your tried with "Allow an higher compat profile for application that request it" enabled
- That your tried to start the application with  MESA_GL_VERSION_OVERRIDE=4.5CCOMPAT environment's variable set


Debugging :

As ususal you can debug like this, for a wine application, (and fill entirely your storage at the same time) :
WINEDEBUG="+opengl" MESA_DEBUG=context MESA_GLSL=log  wine MyApplication.exe |& tee -a logerr

of course you can prepend with a proper MESA_GL_VERSION_OVERRIDE.


Building :

Just pay attention to libdrm's version, (and LLVM/CLANG, eventually)  all should be bleeding edge.


If you run archlinux's derivative, you can use the script buildmesa.sh available in pkgbuild subdirectory it *might help* (but doesn't seem to work with manjaro):
- You need to have at least a [multilib] repository enabled  in /etc/pacman.conf
- You need to have [mesa-git] repository  enabled in /etc/pacman.conf
- You probably need to check your /etc/makepkg.conf options (CFLAGS, CXXFLAGS, LDFLAGS, MAKEFLAGS)


Notes:

 buildmesa.sh isn't supported, and is going to be deprecated in favor of a container based approach.

 This version is unsupported upstream, if it breaks your system, blame me, blame the world, but don't blame people from mesa's developpement team.

 They do an excellent job, and I do not :) [ http://www.phoronix.com/scan.php?page=article&item=amdgpu-1730-radeonsi&num=1 ]

