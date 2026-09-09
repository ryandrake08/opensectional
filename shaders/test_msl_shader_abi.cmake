file(READ "${VERTEX_SHADER}" vertex_source)
file(READ "${FRAGMENT_SHADER}" fragment_source)

if(NOT vertex_source MATCHES "constant SharedUniforms&[^\\n]*\\[\\[buffer\\(0\\)\\]\\]" OR
   NOT vertex_source MATCHES "metadata[^\\n]*\\[\\[buffer\\(1\\)\\]\\]")
    message(FATAL_ERROR "line vertex MSL must bind uniforms at buffer 0 and metadata at buffer 1")
endif()

if(NOT fragment_source MATCHES "constant SharedUniforms&[^\\n]*\\[\\[buffer\\(0\\)\\]\\]" OR
   NOT fragment_source MATCHES "polyline[^\\n]*\\[\\[buffer\\(1\\)\\]\\]" OR
   NOT fragment_source MATCHES "metadata[^\\n]*\\[\\[buffer\\(2\\)\\]\\]")
    message(FATAL_ERROR "line fragment MSL must bind uniforms at buffer 0, points at buffer 1, and metadata at buffer 2")
endif()
