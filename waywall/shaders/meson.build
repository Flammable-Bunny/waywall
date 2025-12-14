# SPIR-V shader compilation for Vulkan

glslc = find_program('glslc', required: false)
glslangValidator = find_program('glslangValidator', required: false)

if glslc.found()
  shader_compiler = glslc
  shader_args = ['-O', '@INPUT@', '-o', '@OUTPUT@']
elif glslangValidator.found()
  shader_compiler = glslangValidator
  shader_args = ['-V', '@INPUT@', '-o', '@OUTPUT@']
else
  error('No SPIR-V compiler found. Install glslc (shaderc) or glslangValidator (glslang).')
endif

shader_sources = [
  'texcopy.vert',
  'texcopy.frag',
  'text.frag',
  'blit.vert',
  'blit.frag',
  'blit_buffer.frag',
]

spirv_shaders = []
foreach shader : shader_sources
  spirv_shaders += custom_target(
    shader + '.spv',
    input: shader,
    output: shader + '.spv',
    command: [shader_compiler] + shader_args,
  )
endforeach

# Generate C header with embedded SPIR-V bytecode
embed_script = files('embed_spirv.py')

shader_embed = custom_target(
  'shader_spirv.h',
  input: spirv_shaders,
  output: 'shader_spirv.h',
  command: [
    find_program('python3'),
    embed_script,
    '@OUTPUT@',
    '@INPUT@',
  ],
)

waywall_shaders = shader_embed
