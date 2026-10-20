// Frame generation's passes in their Vulkan form (runtime/nvn/framegen_src.c framegen_glsl) compile to SPIR-V with
// shaderc, as vulkan/shaders.c compiles them at run time. Linux (shaderc): e.g. in a debian:trixie container,
//   apt-get install -y clang libshaderc-dev
//   cd recomp && clang -I runtime -o /tmp/test_framegen_glsl tools/test_framegen_glsl.c runtime/nvn/framegen_src.c -lshaderc
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <string.h>

#include "nvn/framegen.h"

int main(void) {
    static const char *const passes[] = {"fg_luma.frag", "fg_down.frag", "fg_motion.frag", "fg_median.frag",
                                         "fg_cost_rows.frag", "fg_cost_total.frag", "fg_warp.frag"};
    shaderc_compiler_t c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    int bad = 0;
    for (size_t i = 0; i < sizeof passes / sizeof passes[0]; i++) {
        const char *src = framegen_glsl(passes[i]);
        if (!src) { printf("FAIL %s: no source\n", passes[i]); bad++; continue; }
        shaderc_compilation_result_t r =
            shaderc_compile_into_spv(c, src, strlen(src), shaderc_fragment_shader, passes[i], "main", o);
        bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
        if (ok) printf("PASS %s (%zu bytes SPIR-V)\n", passes[i], shaderc_result_get_length(r));
        else printf("FAIL %s\n%s\n", passes[i], shaderc_result_get_error_message(r));
        bad += !ok;
        shaderc_result_release(r);
    }
    if (framegen_glsl("fg_nope.frag")) { printf("FAIL an unknown pass has a source\n"); bad++; }
    shaderc_compile_options_release(o);
    shaderc_compiler_release(c);
    return bad ? 1 : 0;
}
