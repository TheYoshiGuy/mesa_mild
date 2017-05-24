
This version incorporate some behaviours typical from a COMPATIBILITY PROFILE into a CORE PROFILE.

This version allows some more program to render correctly., 

The new options that are available in drirc (and thus driconf), are  labelled :
- allow_extended_primitive_type
- allow_gl_extensions_in_core
- allow_relaxed_vbo_validation
- allow_minus_one_index_uniform

(Note that actually threading is not working  with these modifications.)

How does it works :

1) In drirc Activate all four options and disable "mesa_glthread" you may want to activate "force_glsl_extensions_warn" if it suits your needs.
NOTE : if you prefer to use driconf, do so, however the way options are labelled slightly differ

2) Force a core context on the environnments or type a command line like this

MESA_GL_VERSION_OVERRIDE=4.5 \<my application  \>
or, if you use wine -
MESA_GL_VERSION_OVERRIDE=4.5 wine \<application\>



