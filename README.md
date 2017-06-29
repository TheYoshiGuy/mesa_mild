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

 - In the binary, replace "#version 420" with "#version 450"
 - Enable "Force GLSL extension behavior to 'warn'" in driconf
 - If you're brave enough to use an intel IGD, you must set

  MESA_VENDOR_OVERRIDE="mesa" MESA_RENDERER_OVERRIDE="mesa"
  (if you endup with a letter box display, it means it's not set)

In all cases, use  :

MESA_GL_VERSION_OVERRIDE=4.5COMPAT wine Cemu.exe



If you run arch linux derivative, you can use the script buildmesa.sh available in pkgbuild subdirectory

There won't be any kind of support if it's not working from my end
