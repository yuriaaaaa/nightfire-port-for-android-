// Os shaders do renderizador Vulkan (render/renderer_vulkan.cpp), em GLSL 4.50 para o SPIR-V. São os de
// renderer_metal.mm (e, por trás deles, os HLSL de renderer_d3d11.cpp e movie_layer_d3d11.cpp) com as mesmas contas na
// mesma ordem; um arquivo só, compilado uma vez por função de entrada pelo glslc com -D<NOME> (o CMakeLists.txt).
//
// O que muda do MSL: as matrizes do motor (vetor-linha, v' = v * M) chegam como estão e o GLSL as lê por colunas como o
// MSL, então "M * v" é o "mul(v, M)" do HLSL; rint() vira roundEven(), fmod() (só com valores positivos aqui) vira mod(),
// saturate() vira clamp(x, 0, 1). O y do recorte do Vulkan cresce para baixo (o do Metal e o do D3D11 para cima): cada
// vertex shader inverte gl_Position.y no fim, e o resto (as coordenadas de pixel, a origem no canto de cima) é igual.
//
// Os recursos: o conjunto 0 é o pedaço dos dados do quadro (os uniform buffers dinâmicos FrameU, ObjectU, a paleta dos
// ossos e MorphU, e o storage buffer dos pixels da composição); o 1, as duas texturas; o 2, os deltas dos morphs da
// malha; as constantes pequenas (névoa, redução, desfoque, filme, retângulo) vão em push constants.

#version 450

layout(set = 0, binding = 0, std140) uniform FrameU {
    mat4 view_proj;
    vec3 light_dir;   // só no sombreamento de depuração (tecla V)
    float ambient;
    float debug_shading;
    float lights_on;  // 0: as rotinas de luz da VU1 viram as sem luz (--no-lights)
    vec2 pad;
} f;

layout(set = 0, binding = 1, std140) uniform ObjectU {
    mat4 world;
    vec4 tint;          // TAmbient: tweak / 255 (r, g, b, a); com luzes, o termo ambiente
    vec4 light_pos[2];  // luzes da VU1 no espaço dos vértices (rígida) ou no mundo (skinning)
    vec4 light_col[2];  // TColor0/1: r, g, b (0..255 x intensidade), w = 1 / raio²
    vec4 env_s;         // linhas 0 e 1 da câmera (mapa de ambiente, rotina 0x2e0)
    vec4 env_t;
    float num_lights;
    float env;          // 1: rotina 0x2e0 (ST pela normal, sem luzes)
    float premult;      // 1: a cor sai multiplicada por As (a mistura usa ONE no lugar de As)
    float sprite;       // 1: sprite de partículas (cor na escala do GS, sem rotina da VU1)
    float atst;         // teste de alfa do GS (ATST 0..7); -1: sem teste
    float aref;         // AREF, na escala do GS (As = 1.0 vale 128)
    float aint;         // 1: compara o alfa inteiro, como o GS; 0: o valor contínuo (regra antiga)
    float screen;       // 1: 2D, a posição já está na tela (x, y em -1..1)
} o;

layout(set = 0, binding = 2, std140) uniform Bones {
    mat4 m[128];  // kMaxSkinBones
} bones;

layout(set = 0, binding = 3, std140) uniform MorphU {
    vec4 w[22];  // kMaxMorphTargets; o peso do alvo k em w[k].x
    uint n;      // alvos
    uint count;  // elementos do buffer dos deltas (fora dele, 0: como a leitura fora de um buffer no D3D11)
    uvec2 pad;
} mu;

layout(set = 0, binding = 4, std430) readonly buffer Pixels {
    uint px[];  // BGRA com o alfa pré-multiplicado, um pixel por palavra
} pixels;

layout(set = 2, binding = 0, std430) readonly buffer MorphDelta {
    float d[];  // float3 sem folga
} morph_delta;

#if !defined(PS_FOG_MS)
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
#endif

// As constantes pequenas (as dos programas de tela cheia, do filme e da composição): os structs do MSL, em 64 bytes.
#if defined(PS_FOG) || defined(PS_FOG_MS)
layout(push_constant, std430) uniform FogU {
    vec4 color;
    float a;
    float b;
    float nf;   // perto * longe da projeção do PC
    float fmn;  // longe - perto
    float n;    // perto
} u;
#elif defined(PS_DOWN)
layout(push_constant, std430) uniform DownU {
    vec2 src_size;
    vec2 ratio;
    float taps;
} u;
#elif defined(PS_BLUR) || defined(PS_BLUR_MIX)
layout(push_constant, std430) uniform BlurU {
    vec2 texel;
    vec2 dir;
    float amount;
} u;
#elif defined(VS_MOVIE) || defined(PS_MOVIE)
layout(push_constant, std430) uniform MovieU {
    vec4 rect;  // x0, y0, x1, y1 em pixels do alvo
    vec2 target;
    vec2 tex_size;
} u;
#elif defined(VS_RECT) || defined(PS_COMPOSITE)
layout(push_constant, std430) uniform RectU {
    vec4 rect;  // x0, y0, x1, y1 em pixels do alvo
    vec2 target;
    uint pitch;  // pixels por linha no buffer
    uint pad;
} u;
#endif

const vec2 kCorner[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 1.0));

mat3 upper3(mat4 m) { return mat3(m[0].xyz, m[1].xyz, m[2].xyz); }

// ---------------------------------------------------------------- os vertex shaders das malhas

#if defined(VS_MAIN) || defined(VS_SKIN) || defined(VS_SKIN_MORPH)
layout(location = 0) in vec3 i_pos;
layout(location = 1) in vec3 i_nrm;
layout(location = 2) in vec2 i_uv;
layout(location = 3) in vec4 i_col;
#if defined(VS_SKIN) || defined(VS_SKIN_MORPH)
layout(location = 4) in uvec4 i_bone;
layout(location = 5) in float i_weight;
#endif
#if defined(VS_SKIN_MORPH)
layout(location = 6) in int i_morph;
#endif

layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_col;  // cor do vértice depois da VU1, com 0x80 = 1.0

// A rotina da VU1 (vu1::tweak_vertex, vu1::light_vertex): sem luzes a cor vezes TAmbient; com luzes (0x340)
// TAmbient * cor + TColor1 * s1 + TColor0 * s0, saturado em 255, s = max(0, max(0, 1 / |D|² - 1 / raio²) * (n . D)).
vec4 vu1_colour(vec4 c, vec3 n, vec3 p) {
    vec4 r = o.tint * c;
    if (o.num_lights > 0.5 && o.env < 0.5 && f.lights_on > 0.5) {
        vec3 d0 = o.light_pos[0].xyz - p;
        vec3 d1 = o.light_pos[1].xyz - p;
        float s0 = max(0.0, max(0.0, 1.0 / dot(d0, d0) - o.light_col[0].w) * dot(n, d0));
        float s1 = max(0.0, max(0.0, 1.0 / dot(d1, d1) - o.light_col[1].w) * dot(n, d1));
        r.rgb = min(r.rgb + o.light_col[1].rgb * s1 + o.light_col[0].rgb * s0, vec3(255.0));
        r.a = min(r.a, 255.0);
    }
    return floor(r);
}

// Rotina 0x2e0: ST = 0.5 + (n . linha da câmera) / 256, com n inteiro (vu1::env_st).
vec2 vu1_env_st(vec3 nvu) { return 0.5 + vec2(dot(nvu, o.env_s.xyz), dot(nvu, o.env_t.xyz)) / 256.0; }
#endif

#if defined(VS_MAIN)
void main() {
    vec4 w = o.world * vec4(i_pos, 1.0);
    gl_Position = f.view_proj * w;
    v_nrm = upper3(o.world) * i_nrm;
    v_uv = i_uv;
    vec4 c = roundEven(i_col * 255.0);  // escala do GS, como no disco
    if (o.screen > 0.5) {
        gl_Position = vec4(i_pos.xy, 0.0, 1.0);
        v_nrm = vec3(0.0, 0.0, 1.0);
        v_col = c / 128.0;
    } else if (o.sprite > 0.5) {
        v_col = c / 128.0;
    } else {
        // Malha rígida: a normal guardada é o inteiro do UNPACK / 128.
        if (o.env > 0.5) v_uv = vu1_env_st(i_nrm * 128.0);
        v_col = vu1_colour(c, i_nrm, i_pos) / 128.0;
    }
    gl_Position.y = -gl_Position.y;
}
#endif

// Skinning (o SkinIt da VU0): w * (p * M0) + (1 - w) * (p * M1); a normal da VU1 é a do disco * 128 girada só pelo
// primeiro osso e truncada (FTOI0). Os rostos: pos += soma em k de peso[k] * delta[m + k] antes (nf::morph_vertex).
#if defined(VS_SKIN) || defined(VS_SKIN_MORPH)
void main() {
    vec3 mp = i_pos;
#if defined(VS_SKIN_MORPH)
    if (i_morph >= 0) {
        for (uint k = 0u; k < mu.n; ++k) {
            uint e = uint(i_morph) + k;
            if (e < mu.count) {
                mp += vec3(morph_delta.d[e * 3u], morph_delta.d[e * 3u + 1u], morph_delta.d[e * 3u + 2u]) * mu.w[k].x;
            }
        }
    }
#endif
    vec4 p = vec4(mp, 1.0);
    mat4 m0 = bones.m[i_bone.x];
    mat4 m1 = bones.m[i_bone.y];
    vec3 sp = i_weight * (m0 * p).xyz + (1.0 - i_weight) * (m1 * p).xyz;
    vec3 sn = i_weight * (upper3(m0) * i_nrm) + (1.0 - i_weight) * (upper3(m1) * i_nrm);
    vec4 w = o.world * vec4(sp, 1.0);
    gl_Position = f.view_proj * w;
    v_nrm = upper3(o.world) * sn;
    v_uv = i_uv;
    vec3 nvu = trunc(upper3(m0) * (i_nrm * 128.0));
    if (o.env > 0.5) v_uv = vu1_env_st(nvu);
    v_col = vu1_colour(roundEven(i_col * 255.0), nvu / 128.0, sp) / 128.0;
    gl_Position.y = -gl_Position.y;
}
#endif

// ---------------------------------------------------------------- os pixel shaders das malhas

#if defined(PS_MAIN) || defined(PS_A2C)
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_col;
layout(location = 0) out vec4 out_color;

// Teste de alfa do GS (TEST_1.ATST) com o alfa a e a referência na escala do GS.
bool alpha_pass(float a) {
    if (o.aint > 0.5) a = floor(a + 0.001);
    int fn = int(o.atst);
    if (fn == 0) return false;
    if (fn == 2) return a < o.aref;
    if (fn == 3) return a <= o.aref;
    if (fn == 4) return a == o.aref;
    if (fn == 5) return a >= o.aref;
    if (fn == 6) return a > o.aref;
    if (fn == 7) return a != o.aref;
    return true;
}
#endif

// MODULATE do GS com a cor do vértice: Cf = Ct * Cv / 128 (saturada), Af = At * Av / 128 (até 0xff); As = Af / 128.
#if defined(PS_MAIN)
void main() {
    vec4 c = texture(tex0, v_uv);
    float a = min(c.a * (255.0 / 128.0) * v_col.a, 255.0 / 128.0);
    vec3 lit = clamp(c.rgb * v_col.rgb, 0.0, 1.0);
    if (f.debug_shading > 0.5 && o.sprite < 0.5) {
        float ndl = abs(dot(normalize(v_nrm), -normalize(f.light_dir)));
        lit = c.rgb * (f.ambient + (1.0 - f.ambient) * ndl) * o.tint.rgb;
    }
    if (o.atst >= 0.0 && !alpha_pass(a * 128.0)) discard;
    if (o.premult > 0.5) lit *= a;
    // O alfa vai para o alvo em todos os desenhos, como no GS (fator Ad das misturas seguintes).
    out_color = vec4(lit, clamp(a, 0.0, 1.0));
}
#endif

// Com MSAA: o alfa para a cobertura em volta do limiar do teste (renderer_d3d11.cpp, ps_a2c).
#if defined(PS_A2C)
void main() {
    vec4 c = texture(tex0, v_uv);
    float a = min(c.a * (255.0 / 128.0) * v_col.a, 255.0 / 128.0);
    vec3 lit = clamp(c.rgb * v_col.rgb, 0.0, 1.0);
    if (f.debug_shading > 0.5 && o.sprite < 0.5) {
        float ndl = abs(dot(normalize(v_nrm), -normalize(f.light_dir)));
        lit = c.rgb * (f.ambient + (1.0 - f.ambient) * ndl) * o.tint.rgb;
    }
    float a128 = a * 128.0;
    float t = o.aref;
    if (o.aint > 0.5) t += (int(o.atst) == 6 ? 1.0 : 0.0) - 0.001;
    float w = max(fwidth(a128), 0.001);
    float bias = t > 127.0 ? 1.0 : 0.5;
    float cov = clamp((a128 - t) / w + bias, 0.0, 1.0);
    if (cov <= 0.0) discard;
    out_color = vec4(lit, cov);
}
#endif

// ---------------------------------------------------------------- tela cheia, filme e composição

// Um triângulo que cobre o alvo.
#if defined(VS_FULL)
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    gl_Position.y = -gl_Position.y;
}
#endif

#if !defined(VS_MAIN) && !defined(VS_SKIN) && !defined(VS_SKIN_MORPH) && !defined(PS_MAIN) && !defined(PS_A2C) && \
    !defined(VS_FULL) && !defined(VS_MOVIE) && !defined(VS_RECT)
layout(location = 0) out vec4 out_color;
#endif

// Passada de névoa (psiDrawFogPass): o Z do GS de cada pixel dá o quanto da cor fica (vu1::gs_z, vu1::fog_keep).
#if defined(PS_FOG) || defined(PS_FOG_MS)
vec4 fog_of(float d) {
    float z = 1.0;
    if (d > 0.0) {
        float view_z = u.nf / (d * u.fmn + u.n);  // profundidade invertida
        float s = u.a / view_z + u.b;
        z = s > 0.0 ? mod(floor(s), 16777216.0) : 0.0;
    }
    float keep = z >= 32767.0 ? 1.0 : floor(z / 256.0) / 128.0;
    return vec4(u.color.rgb, keep);
}
#endif
#if defined(PS_FOG)
void main() { out_color = fog_of(texelFetch(tex0, ivec2(gl_FragCoord.xy), 0).r); }
#endif
// Com MSAA: por amostra, com a profundidade de cada uma.
#if defined(PS_FOG_MS)
layout(set = 1, binding = 0) uniform sampler2DMS depth_ms;
void main() { out_color = fog_of(texelFetch(depth_ms, ivec2(gl_FragCoord.xy), gl_SampleID).r); }
#endif

// Cópias: o texel do pixel (o mesmo tamanho; também as texturas comprimidas, que o Metal amostra pelo pixel).
#if defined(PS_BLIT)
void main() { out_color = texelFetch(tex0, ivec2(gl_FragCoord.xy), 0); }
#endif

// A cena em escala reduzida para a janela (ps_down do D3D11): taps x taps amostras bilineares sobre a área do pixel.
#if defined(PS_DOWN)
void main() {
    vec2 start = (gl_FragCoord.xy - 0.5) * u.ratio;
    vec2 step = u.ratio / u.taps;
    int n = int(u.taps);
    vec4 sum = vec4(0.0);
    for (int j = 0; j < n; ++j) {
        for (int k = 0; k < n; ++k) {
            vec2 t = start + (vec2(float(k), float(j)) + 0.5) * step;
            sum += textureLod(tex0, t / u.src_size, 0.0);
        }
    }
    out_color = sum / (u.taps * u.taps);
}
#endif

// O fundo desfocado dos menus Modern: a gaussiana de 9 amostras em 5 leituras bilineares e a mistura com a imagem nítida.
#if defined(PS_BLUR)
void main() {
    vec2 uv = gl_FragCoord.xy * u.texel;
    vec2 d = u.dir * u.texel;
    vec4 c = textureLod(tex0, uv, 0.0) * 0.2270270270;
    c += textureLod(tex0, uv + d * 1.3846153846, 0.0) * 0.3162162162;
    c += textureLod(tex0, uv - d * 1.3846153846, 0.0) * 0.3162162162;
    c += textureLod(tex0, uv + d * 3.2307692308, 0.0) * 0.0702702703;
    c += textureLod(tex0, uv - d * 3.2307692308, 0.0) * 0.0702702703;
    out_color = c;
}
#endif
#if defined(PS_BLUR_MIX)
void main() {
    vec4 sharp = texelFetch(tex0, ivec2(gl_FragCoord.xy), 0);
    vec4 soft = textureLod(tex1, gl_FragCoord.xy * u.texel, 0.0);
    out_color = mix(sharp, soft, u.amount);
}
#endif

// O quadro do filme (movie_layer_d3d11): a imagem no retângulo da tela do PS2 com o Catmull-Rom separável em 9 leituras.
#if defined(VS_MOVIE)
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 c = kCorner[gl_VertexIndex];
    vec2 p = mix(u.rect.xy, u.rect.zw, c);
    gl_Position = vec4(p.x / u.target.x * 2.0 - 1.0, 1.0 - p.y / u.target.y * 2.0, 0.0, 1.0);
    gl_Position.y = -gl_Position.y;
    v_uv = c;
}
#endif
#if defined(PS_MOVIE)
layout(location = 0) in vec2 v_uv;
void main() {
    vec2 at = v_uv * u.tex_size;
    vec2 c1 = floor(at - 0.5) + 0.5;
    vec2 fr = at - c1;
    vec2 w0 = fr * (-0.5 + fr * (1.0 - 0.5 * fr));
    vec2 w1 = 1.0 + fr * fr * (-2.5 + 1.5 * fr);
    vec2 w2 = fr * (0.5 + fr * (2.0 - 1.5 * fr));
    vec2 w3 = fr * fr * (-0.5 + 0.5 * fr);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c1 - 1.0) / u.tex_size;
    vec2 t12 = (c1 + w2 / w12) / u.tex_size;
    vec2 t3 = (c1 + 2.0) / u.tex_size;
    vec3 c = vec3(0.0);
    c += textureLod(tex0, vec2(t0.x, t0.y), 0.0).rgb * (w0.x * w0.y);
    c += textureLod(tex0, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y);
    c += textureLod(tex0, vec2(t3.x, t0.y), 0.0).rgb * (w3.x * w0.y);
    c += textureLod(tex0, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y);
    c += textureLod(tex0, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y);
    c += textureLod(tex0, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y);
    c += textureLod(tex0, vec2(t0.x, t3.y), 0.0).rgb * (w0.x * w3.y);
    c += textureLod(tex0, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    c += textureLod(tex0, vec2(t3.x, t3.y), 0.0).rgb * (w3.x * w3.y);
    out_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
#endif

// Uma imagem da UiVectorCanvas (BGRA, alfa pré-multiplicado) lida texel a texel de um buffer, no retângulo dela.
#if defined(VS_RECT)
void main() {
    vec2 p = mix(u.rect.xy, u.rect.zw, kCorner[gl_VertexIndex]);
    gl_Position = vec4(p.x / u.target.x * 2.0 - 1.0, 1.0 - p.y / u.target.y * 2.0, 0.0, 1.0);
    gl_Position.y = -gl_Position.y;
}
#endif
#if defined(PS_COMPOSITE)
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) - ivec2(u.rect.xy);
    int w = int(u.rect.z - u.rect.x);
    int h = int(u.rect.w - u.rect.y);
    if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h) discard;
    uint idx = uint(clamp(p.y, 0, h - 1)) * u.pitch + uint(clamp(p.x, 0, w - 1));
    out_color = unpackUnorm4x8(pixels.px[idx]).zyxw;
}
#endif
