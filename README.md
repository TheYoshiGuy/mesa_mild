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




