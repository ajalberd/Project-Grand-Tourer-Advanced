"""Run after ppu_lifter.py, before building:  python -X utf8 tools\\post_lift.py recompiled [--trace <hex>,<hex>,...]
Applies the GT6 port's changes to the freshly lifted sources (see gt6_recomp_hooks.h). Safe to run twice.

--trace  (diagnostic) wraps the named guest functions so that EVERY call -- direct calls from lifted code included,
         which GT6_TRACE_FN cannot see -- is logged by gt6_trace_enter / gt6_trace_leave (gt6_host.cpp) when the run
         has GT6_DTRACE=1. The set given replaces the previous one: run without --trace (or with an empty list) to
         remove all wrappers. Only the chunks that change are rewritten, so only those recompile."""
import glob, os, re, sys

RENAME = {                       # lifted body -> name it keeps; the port supplies the original name
    'func_00C61F88': 'func_00C61F88_guest',     # setjmp
    'func_00C620AC': 'func_00C620AC_guest',     # longjmp
    'func_00482914': 'func_00482914_guest',     # the race's driver-change step (a host hook point, gt6_host.cpp)
}
HOOK_INCLUDE = '#include "gt6_recomp_hooks.h"'
# GT6_NO_MLAA (gt6_host.cpp): four guarded sites from the EMAIN 1.22 Disable MLAA patch.
# main.cpp defaults to GPU AA; GT6_AA=spu selects the original job path. Each site occurs exactly once.
CALL = '        ctx->lr = 0x{:08X}; {}(ctx); DRAIN_TRAMPOLINE(ctx);\n'
NOMLAA = [
    (CALL.format(0x008C2008, 'func_00C2FED4'),      # 0x008C2004  copy=1: stage the scene for SPU access
     '        ctx->lr = 0x008C2008; if (!gt6_no_spu_post()) { func_00C2FED4(ctx); DRAIN_TRAMPOLINE(ctx); } /*GT6NOMLAA*/\n'),
    (CALL.format(0x008C2064, 'func_00C2FED4'),      # 0x008C2060  copy=0: submit MLAA, replaced by a GPU scene copy
     '        ctx->lr = 0x008C2064; if (!gt6_no_spu_post()) { func_00C2FED4(ctx); DRAIN_TRAMPOLINE(ctx); }'
     ' else if (gt6_scene_copy_args(ctx)) { func_00C3E050(ctx); DRAIN_TRAMPOLINE(ctx); gt6_scene_copy_done(ctx); } /*GT6NOMLAA*/\n'),
    (CALL.format(0x008C2084, 'func_00C2FD90'),      # 0x008C2080  bl 0xc2fd90   the wait for the job
     '        ctx->lr = 0x008C2084; if (!gt6_no_spu_post()) { func_00C2FD90(ctx); DRAIN_TRAMPOLINE(ctx); } /*GT6NOMLAA*/\n'),
]
# 0x008C3044  beq cr7, 0x8c3084 after lbz r0, 0x31(r31): with no post-process the GPU glare composite always runs.
NOMLAA_BR = re.compile(r'(ctx->gpr\[0\] = vm_read8\(ctx->gpr\[31\] \+ 0x31\);\n[^\n]*\n\s*)'
                       r'if \(\(\(ctx->cr >> 1\) & 1\)\) goto loc_008C3084;\n')
NOMLAA_BR_NEW = r'\1if (((ctx->cr >> 1) & 1) && !gt6_no_spu_post()) goto loc_008C3084; /*GT6NOMLAA*/' + '\n'
nomlaa_done = [0] * (len(NOMLAA) + 1)
TRACE_BLOCK = re.compile(r'/\*GT6TRACE (func_[0-9A-F]{8})\*/.*?/\*GT6TRACE-END\*/\n', re.S)
DOF_DIAG_HEAD = 'void func_008C2970(ppu_context* ctx) {\n'
DOF_DIAG_DECL = 'extern "C" int gt6_diag_skip_dof(ppu_context* ctx);\n'
DOF_DIAG_GUARD = '    if (gt6_diag_skip_dof(ctx)) return; /*GT6_DIAG_DOF*/\n'
dof_diag_done = 0
AUDIO_FLOW_CALL = CALL.format(0x00CB87D8, 'func_00CB59E8')
AUDIO_FLOW_HOOK = '        { extern void gt6_audio_trace_completed(ppu_context*); gt6_audio_trace_completed(ctx); } /*GT6_AUDIO_FLOW*/\n'
audio_flow_done = 0
ASPECT_CALL = CALL.format(0x008A615C, 'func_008FF168')
ASPECT_HOOK = '        { extern void gt6_aspect_override(ppu_context*); gt6_aspect_override(ctx); } /*GT6_ASPECT*/\n'
aspect_done = 0
AUDIO_FIFO_BEGIN = '        { extern void gt6_audio_output_begin(ppu_context*); gt6_audio_output_begin(ctx); } /*GT6_AUDIO_FIFO_BEGIN*/\n'
AUDIO_FIFO_END = '        { extern void gt6_audio_output_complete(ppu_context*); gt6_audio_output_complete(ctx); } /*GT6_AUDIO_FIFO_END*/\n'
audio_fifo_done = 0

FRAME_WAIT_CALL = CALL.format(0x008A8A48, 'func_008A7D5C')
FRAME_WAIT_HOOK = ('        ctx->lr = 0x008A8A48; gt6_render_wait_target(ctx); '
                   'func_008A7D5C(ctx); DRAIN_TRAMPOLINE(ctx); /*GT6_RENDER_WAIT*/\n')
FLIP_TARGET_OLD = (
    '        ctx->gpr[0] = (uint64_t)((int64_t)(int32_t)ctx->gpr[28] * (int64_t)(int32_t)ctx->gpr[0]);\n'
    '        ctx->gpr[0] = (int64_t)(int32_t)ctx->gpr[0];\n'
    '        ctx->gpr[0] = ctx->gpr[27] + ctx->gpr[0];\n'
    '        vm_write64(ctx->gpr[29] + 0x10, ctx->gpr[0]);\n'
)
FLIP_TARGET_NEW = (
    '        ctx->gpr[0] = (uint64_t)((int64_t)(int32_t)ctx->gpr[28] * (int64_t)(int32_t)ctx->gpr[0]);\n'
    '        ctx->gpr[0] = (int64_t)(int32_t)ctx->gpr[0];\n'
    '        ctx->gpr[0] = ctx->gpr[27] + ctx->gpr[0];\n'
    '        { extern void gt6_flip_target_vblank(ppu_context*); gt6_flip_target_vblank(ctx); } /*GT6_FLIP_TARGET*/\n'
    '        vm_write64(ctx->gpr[29] + 0x10, ctx->gpr[0]);\n'
)
FRAME_TIME_HEAD = 'void func_001F90FC(ppu_context* ctx) {\n'
FRAME_TIME_HOOK = '    gt6_frame_timing_observe(ctx); /*GT6_FRAME_TIMING*/\n'
frame_wait_done = 0
flip_target_done = 0
frame_time_done = 0
movie_post_done = 0
movie_clock_done = 0

MOVIE_CLOCK_INCLUDE = '#include "gt6_movie_clock.h"\n'
MOVIE_CLOCK_HOOKS = [
    ('GT6_MOVIE_CLOCK_BEGIN', CALL.format(0x008AC134, 'func_00B10A84'),
     '        gt6_movie_clock_begin((uint32_t)ctx->gpr[29], (uint32_t)ctx->gpr[30], ctx->gpr[28], ctx->gpr[3], (uint32_t)vm_read64(ctx->gpr[1] + 0xB0), (uint32_t)vm_read64(ctx->gpr[1] + 0x98)); /*GT6_MOVIE_CLOCK_BEGIN*/\n'),
    ('GT6_MOVIE_CLOCK_FINISH', 'loc_008AC170:\n',
     '        ctx->gpr[31] = gt6_movie_clock_finish(ctx->gpr[31]); /*GT6_MOVIE_CLOCK_FINISH*/\n'),
    ('GT6_MOVIE_CLOCK_SEEK', 'loc_008AC228:\n',
     '        gt6_movie_clock_seek((uint32_t)ctx->gpr[29], ctx->gpr[27], (uint32_t)ctx->gpr[31], ctx->gpr[0]); /*GT6_MOVIE_CLOCK_SEEK*/\n'),
    ('GT6_MOVIE_CLOCK_PCM', 'loc_008AC84C:\n',
     '        gt6_movie_clock_produce((uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[27], vm_read32(ctx->gpr[1] + 0x100), (uint32_t)ctx->gpr[6]); /*GT6_MOVIE_CLOCK_PCM*/\n'),
]


def apply_movie_clock(text):
    _, begin_anchor, begin_hook = MOVIE_CLOCK_HOOKS[0]
    previous_begin_hook = ('        gt6_movie_clock_begin((uint32_t)ctx->gpr[29], (uint32_t)ctx->gpr[30], '
                           'ctx->gpr[28], ctx->gpr[3], (uint32_t)vm_read64(ctx->gpr[1] + 0xB0)); /*GT6_MOVIE_CLOCK_BEGIN*/\n')
    if previous_begin_hook in text:
        if text.count(begin_anchor + previous_begin_hook) != 1 or begin_hook in text:
            raise ValueError('ambiguous movie player diagnostic hook migration')
        text = text.replace(begin_anchor + previous_begin_hook, begin_anchor + begin_hook)
    # Earlier diagnostics observed only the split-AU branch. Normal 2048-sample
    # movie AUs use the first 256-sample fragment at 008AC84C.
    _, pcm_anchor, pcm_hook = MOVIE_CLOCK_HOOKS[-1]
    previous_pcm_anchor = 'loc_008AC8D4:\n'
    if previous_pcm_anchor + pcm_hook in text:
        if text.count(previous_pcm_anchor + pcm_hook) != 1 or text.count(pcm_anchor) != 1 or pcm_anchor + pcm_hook in text:
            raise ValueError('ambiguous movie PCM hook migration')
        text = text.replace(previous_pcm_anchor + pcm_hook, previous_pcm_anchor)
        text = text.replace(pcm_anchor, pcm_anchor + pcm_hook)
    counts = [text.count(anchor) for _, anchor, _ in MOVIE_CLOCK_HOOKS]
    markers = [text.count('/*' + name + '*/') for name, _, _ in MOVIE_CLOCK_HOOKS]
    if not any(counts) and not any(markers):
        return text, 0
    if counts != [1] * 4:
        raise ValueError('ambiguous movie clock hook sites')
    if any(markers):
        if markers != [1] * 4 or text.count(MOVIE_CLOCK_INCLUDE) != 1 or any(
                anchor + hook not in text for _, anchor, hook in MOVIE_CLOCK_HOOKS):
            raise ValueError('partial or moved movie clock hooks')
        return text, 4
    text = text.replace('#include "ppu_recomp.h"\n', '#include "ppu_recomp.h"\n' + MOVIE_CLOCK_INCLUDE, 1)
    for _, anchor, hook in MOVIE_CLOCK_HOOKS:
        text = text.replace(anchor, anchor + hook)
    return text, 4

MOVIE_POST_INCLUDE = '#include "gt6_movie_post.h"\n'
MOVIE_POST_BEGIN = '        ctx->gpr[29] = ctx->gpr[9] + (int64_t)(0x200);\nloc_00FD6900:\n'
MOVIE_POST_END = '        if ((!((ctx->cr >> 1) & 1))) goto loc_00FD6930;\n'
MOVIE_POST_BEFORE_OLD = '        gt6_movie_post_before(); /*GT6_MOVIE_POST_BEGIN*/\n'
MOVIE_POST_AFTER_OLD = '        gt6_movie_post_after(); /*GT6_MOVIE_POST_END*/\n'
MOVIE_POST_BEFORE = '        if (gt6_movie_post_before()) goto loc_00FD6950; /*GT6_MOVIE_POST_BEGIN*/\n'
MOVIE_POST_AFTER = 'loc_00FD6950:\n        gt6_movie_post_after(); /*GT6_MOVIE_POST_END*/\n'


def apply_movie_post(text):
    if MOVIE_POST_BEFORE_OLD + MOVIE_POST_BEGIN in text and MOVIE_POST_END + MOVIE_POST_AFTER_OLD in text:
        text = text.replace(MOVIE_POST_BEFORE_OLD, MOVIE_POST_BEFORE).replace(MOVIE_POST_END + MOVIE_POST_AFTER_OLD, MOVIE_POST_END + MOVIE_POST_AFTER)
    markers = [text.count('/*GT6_MOVIE_POST_BEGIN*/'), text.count('/*GT6_MOVIE_POST_END*/')]
    if any(markers):
        if markers != [1, 1] or text.count(MOVIE_POST_INCLUDE) != 1 or \
                MOVIE_POST_BEFORE + MOVIE_POST_BEGIN not in text or MOVIE_POST_END + MOVIE_POST_AFTER not in text:
            raise ValueError('partial or moved movie postprocess hooks')
        return text, 2
    if MOVIE_POST_BEGIN not in text and MOVIE_POST_END not in text:
        return text, 0
    if text.count(MOVIE_POST_BEGIN) != 1 or text.count(MOVIE_POST_END) != 1:
        raise ValueError('ambiguous movie postprocess hooks')
    text = text.replace('#include "ppu_recomp.h"\n', '#include "ppu_recomp.h"\n' + MOVIE_POST_INCLUDE, 1)
    text = text.replace(MOVIE_POST_BEGIN, MOVIE_POST_BEFORE + MOVIE_POST_BEGIN)
    text = text.replace(MOVIE_POST_END, MOVIE_POST_END + MOVIE_POST_AFTER)
    return text, 2


def apply_frame_timing(text):
    """One logical wait call has two overlapping lifted entry bodies."""
    wait_count = text.count(FRAME_WAIT_HOOK)
    flip_count = text.count(FLIP_TARGET_NEW)
    time_count = text.count(FRAME_TIME_HEAD + FRAME_TIME_HOOK)
    if text.count('/*GT6_RENDER_WAIT*/') != wait_count:
        raise ValueError('moved or partial render wait hook')
    if text.count('/*GT6_FLIP_TARGET*/') != flip_count:
        raise ValueError('moved or partial flip target hook')
    if text.count('/*GT6_FRAME_TIMING*/') != time_count:
        raise ValueError('moved or partial frame timing hook')
    wait_original = text.count(FRAME_WAIT_CALL)
    if wait_count and wait_original:
        raise ValueError('partially patched render wait')
    flip_original = text.count(FLIP_TARGET_OLD)
    if flip_count and flip_original:
        raise ValueError('partially patched flip target')
    text = text.replace(FRAME_WAIT_CALL, FRAME_WAIT_HOOK)
    text = text.replace(FLIP_TARGET_OLD, FLIP_TARGET_NEW)
    if not time_count:
        time_count = text.count(FRAME_TIME_HEAD)
        text = text.replace(FRAME_TIME_HEAD, FRAME_TIME_HEAD + FRAME_TIME_HOOK)
    return text, wait_count + wait_original, flip_count + flip_original, time_count


def apply_audio_fifo(text):
    markers = [text.count('/*GT6_AUDIO_FIFO_BEGIN*/'), text.count('/*GT6_AUDIO_FIFO_END*/')]
    sequence = AUDIO_FLOW_CALL + AUDIO_FLOW_HOOK
    patched = AUDIO_FIFO_BEGIN + sequence + AUDIO_FIFO_END
    if any(markers):
        if markers != [1, 1] or text.count(patched) != 1:
            raise ValueError('partial or moved SGX FIFO hooks')
        return text, 2
    count = text.count(sequence)
    if count > 1:
        raise ValueError('ambiguous SGX submission hook')
    return (text.replace(sequence, patched), 2) if count else (text, 0)

BGM_TRACE_DECL = 'extern "C" void gt6_bgm_diag_trace(ppu_context* ctx, unsigned stage);\n'
BGM_TRACE_HEAD = re.compile(r'^void func_00CC29A8(?:_guest)?\(ppu_context\* ctx\) \{\n', re.M)
BGM_TRACE_CALLS = (
    (1, 0x00CC2A20, 'func_00CBBDE0'),
    (2, 0x00CC2BCC, 'func_00CC7E5C'),
    (3, 0x00CC2F50, 'func_00CAF668'),
    (4, 0x00CC3010, 'func_00CCBD24'),
    (5, 0x00CC2A4C, 'func_00CCBD24'),
)
bgm_trace_done = 0

def bgm_hook(stage):
    return f'        gt6_bgm_diag_trace(ctx, {stage}); /*GT6_BGM_TRACE_{stage}*/\n'

def apply_bgm_trace(text):
    """Observe codec start without changing guest state; reject partial patches."""
    heads = list(BGM_TRACE_HEAD.finditer(text))
    if not heads:
        if '/*GT6_BGM_TRACE_' in text:
            raise ValueError('BGM trace markers without target body')
        return text, 0
    if len(heads) != 1:
        raise ValueError('ambiguous CC29A8 body')
    existing = [text.count(f'/*GT6_BGM_TRACE_{stage}*/') for stage in range(6)]
    if any(existing):
        if existing != [1]*6 or text.count(BGM_TRACE_DECL) != 1:
            raise ValueError('partial BGM trace patch')
        if heads[0].group() + bgm_hook(0) not in text:
            raise ValueError('BGM entry marker moved')
        for stage, lr, function in BGM_TRACE_CALLS:
            call = f'        ctx->lr = 0x{lr:08X}; {function}(ctx); DRAIN_TRAMPOLINE(ctx);\n'
            if call + bgm_hook(stage) not in text:
                raise ValueError(f'BGM stage {stage} moved')
        return text, 6
    text = text[:heads[0].start()] + BGM_TRACE_DECL + text[heads[0].start():]
    text = BGM_TRACE_HEAD.sub(lambda m: m.group() + bgm_hook(0), text)
    for stage, lr, function in BGM_TRACE_CALLS:
        call = f'        ctx->lr = 0x{lr:08X}; {function}(ctx); DRAIN_TRAMPOLINE(ctx);\n'
        if text.count(call) != 1:
            raise ValueError(f'expected exactly one BGM stage {stage} call')
        text = text.replace(call, call + bgm_hook(stage))
    return text, 6

FILE_HEAD = re.compile(r'^void func_00CAF668(?:_guest)?\(ppu_context\* ctx\) \{\n', re.M)
FILE_DECL = ('extern "C" int gt6_sgx_file_handle_is_pointer(ppu_context*);\n'
             'extern "C" void gt6_bgm_diag_trace(ppu_context*, unsigned);\n')
FILE_CALL = '        ctx->lr = 0x00CAF7D4; ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);\n'
FILE_HOOK = '        gt6_bgm_diag_trace(ctx, 6); /*GT6_BGM_FILE_RETURN*/\n'
FILE_BRANCH = '        if (((ctx->cr >> 3) & 1)) goto loc_00CAF8C0;\n'
FILE_BRANCH_NEW = '        if (((ctx->cr >> 3) & 1) && !gt6_sgx_file_handle_is_pointer(ctx)) goto loc_00CAF8C0; /*GT6_SGX_FILE_HANDLE*/\n'
bgm_file_done = 0


def apply_bgm_file_handle(text):
    """Keep the known custom callback's valid high guest pointers as handles."""
    heads = list(FILE_HEAD.finditer(text))
    markers = [text.count('/*GT6_BGM_FILE_RETURN*/'), text.count('/*GT6_SGX_FILE_HANDLE*/')]
    if not heads:
        if any(markers):
            raise ValueError('file markers without CAF668 body')
        return text, 0
    if len(heads) != 1:
        raise ValueError('ambiguous CAF668 body')
    if any(markers):
        if (markers != [1, 1] or text.count(FILE_DECL) != 1 or
                FILE_CALL + FILE_HOOK not in text or FILE_BRANCH_NEW not in text):
            raise ValueError('partial or moved file-handle patch')
        return text, 2
    if text.count(FILE_CALL) != 1 or text.count(FILE_BRANCH) != 1:
        raise ValueError('file hook/branch count mismatch')
    pos = heads[0].start()
    text = text[:pos] + FILE_DECL + text[pos:]
    text = text.replace(FILE_CALL, FILE_CALL + FILE_HOOK).replace(FILE_BRANCH, FILE_BRANCH_NEW)
    return text, 2


# GT6_SPURS_SPUS=<5..8> (gt6_host.cpp): the SPU count of the game's own SPURS instance. The boot code asks for five
# (0x008B07B0 `li r3,5`), the helper it calls clamps the request to five (0x00AF2C2C `li r4,5`), and every job list
# is added through func_00B31A00 with a priority table and a maximum contention made for five. The three places
# ask the port; with the switch unset it answers five and leaves the workload attribute alone.
SPURS_ASK_OLD = ('        ctx->gpr[3] = (int64_t)(int32_t)(5);\n'
                 '        ctx->lr = 0x008B07B8; func_00AF2B84(ctx); DRAIN_TRAMPOLINE(ctx);\n')
SPURS_ASK_NEW = ('        { extern int gt6_spurs_spus(void); ctx->gpr[3] = (int64_t)(int32_t)gt6_spurs_spus(); } /*GT6_SPURS_ASK*/\n'
                 '        ctx->lr = 0x008B07B8; func_00AF2B84(ctx); DRAIN_TRAMPOLINE(ctx);\n')
SPURS_CLAMP_OLD = '        ctx->gpr[4] = (int64_t)(int32_t)(5);\nloc_00AF2C30:\n'
SPURS_CLAMP_NEW = ('        { extern int gt6_spurs_spus(void); ctx->gpr[4] = (int64_t)(int32_t)gt6_spurs_spus(); } /*GT6_SPURS_CLAMP*/\n'
                   'loc_00AF2C30:\n')
SPURS_LIST_CALL = '        ctx->lr = 0x00B31AE0; func_010BF920(ctx); DRAIN_TRAMPOLINE(ctx);\n'
SPURS_LIST_HOOK = '        { extern void gt6_spurs_joblist(ppu_context*); gt6_spurs_joblist(ctx); } /*GT6_SPURS_JOBLIST*/\n'
SPURS_RULES = ((SPURS_ASK_OLD, SPURS_ASK_NEW, '/*GT6_SPURS_ASK*/'), (SPURS_CLAMP_OLD, SPURS_CLAMP_NEW, '/*GT6_SPURS_CLAMP*/'),
               (SPURS_LIST_CALL, SPURS_LIST_CALL + SPURS_LIST_HOOK, '/*GT6_SPURS_JOBLIST*/'))
spurs_done = 0


def apply_spurs_spus(text):
    n = 0
    for old, new, mark in SPURS_RULES:
        if mark in text:
            if text.count(mark) != 1 or text.count(new) != 1:
                raise ValueError('partial or moved ' + mark)
            n += 1
        elif old in text:
            if text.count(old) != 1:
                raise ValueError('ambiguous ' + mark)
            text = text.replace(old, new)
            n += 1
    return text, n


args = sys.argv[1:]
trace = set()
if '--trace' in args:
    i = args.index('--trace')
    trace = {f'func_{int(x, 16):08X}' for x in args[i + 1].split(',') if x.strip()} if i + 1 < len(args) else set()
    del args[i:i + 2]
d = args[0]
if trace & set(RENAME):
    raise SystemExit('setjmp/longjmp are hooked already; they cannot be traced this way')

done = {k: 0 for k in RENAME}
traced = set()
tail_calls = []
for p in sorted(glob.glob(os.path.join(d, 'ppu_recomp_*.cpp'))):
    t = open(p, encoding='utf-8').read()
    o = t
    for a, b in RENAME.items():
        if f'void {b}(ppu_context* ctx) {{' in t:
            done[a] += 1
        elif f'void {a}(ppu_context* ctx) {{' in t:
            t = t.replace(f'void {a}(ppu_context* ctx) {{', f'void {b}(ppu_context* ctx) {{')
            done[a] += 1
    for k, (a, b) in enumerate(NOMLAA):
        if a in t:
            t = t.replace(a, b)
            nomlaa_done[k] += 1
        elif b in t:
            nomlaa_done[k] += 1
    if '&& !gt6_no_spu_post()) goto loc_008C3084; /*GT6NOMLAA*/' in t:
        nomlaa_done[-1] += 1
    else:
        t, n = NOMLAA_BR.subn(NOMLAA_BR_NEW, t)
        nomlaa_done[-1] += n
    # Diagnostic whole-command bypass, before any guest stack/register write.
    # Existing DoF option guards may still submit this command in its other mode.
    if '/*GT6_DIAG_DOF*/' in t:
        dof_diag_done += t.count('/*GT6_DIAG_DOF*/')
    elif DOF_DIAG_HEAD in t:
        t = t.replace(DOF_DIAG_HEAD, DOF_DIAG_DECL + DOF_DIAG_HEAD + DOF_DIAG_GUARD)
        dof_diag_done += 1
    # Trace wrappers: drop the ones no longer wanted, add the new ones.
    if '/*GT6_AUDIO_FLOW*/' in t:
        audio_flow_done += t.count('/*GT6_AUDIO_FLOW*/')
    elif AUDIO_FLOW_CALL in t:
        t = t.replace(AUDIO_FLOW_CALL, AUDIO_FLOW_CALL + AUDIO_FLOW_HOOK)
        audio_flow_done += 1
    if '/*GT6_ASPECT*/' in t:
        aspect_done += t.count('/*GT6_ASPECT*/')
    elif ASPECT_CALL in t:
        t = t.replace(ASPECT_CALL, ASPECT_CALL + ASPECT_HOOK)
        aspect_done += 1
    t, audio_fifo_count = apply_audio_fifo(t)
    audio_fifo_done += audio_fifo_count
    t, frame_wait_count, flip_target_count, frame_time_count = apply_frame_timing(t)
    frame_wait_done += frame_wait_count
    flip_target_done += flip_target_count
    frame_time_done += frame_time_count
    t, movie_post_count = apply_movie_post(t)
    movie_post_done += movie_post_count
    t, movie_clock_count = apply_movie_clock(t)
    movie_clock_done += movie_clock_count
    t, bgm_count = apply_bgm_trace(t)
    bgm_trace_done += bgm_count
    t, bgm_file_count = apply_bgm_file_handle(t)
    bgm_file_done += bgm_file_count
    t, spurs_count = apply_spurs_spus(t)
    spurs_done += spurs_count
    for m in list(TRACE_BLOCK.finditer(t)):
        f = m.group(1)
        if f not in trace:
            t = t.replace(m.group(0), '').replace(f'void {f}_guest(ppu_context* ctx) {{', f'void {f}(ppu_context* ctx) {{')
        else:
            traced.add(f)
    for f in sorted(trace - traced):
        head = f'void {f}(ppu_context* ctx) {{'
        if head in t:
            wrap = (f'/*GT6TRACE {f}*/\nvoid {f}_guest(ppu_context* ctx);\n'
                    f'void {f}(ppu_context* ctx) {{ gt6_trace_enter(0x{f[5:]}u, ctx); {f}_guest(ctx); gt6_trace_leave(0x{f[5:]}u, ctx); }}\n'
                    f'/*GT6TRACE-END*/\n')
            t = t.replace(head, wrap + f'void {f}_guest(ppu_context* ctx) {{')
            traced.add(f)
    # A host setjmp must run in a frame that is still alive at the longjmp: a call site that is a tail call
    # (`{ func_00C61F88(ctx); return; }`) or a trampoline would set it up in a frame that returns at once.
    for m in re.finditer(r'^.*func_00C61F88\b(?!_guest).*$', t, re.M):
        s = m.group(0).strip()
        if not re.match(r'^ctx->lr = 0x[0-9A-F]{8}; func_00C61F88\(ctx\); DRAIN_TRAMPOLINE\(ctx\);$', s) and '"func_00C61F88"' not in s:
            tail_calls.append(f'{os.path.basename(p)}: {s[:140]}')
    if t != o:
        open(p, 'w', encoding='utf-8', newline='').write(t)
        print('patched', os.path.basename(p))
h = os.path.join(d, 'ppu_recomp.h')
t = open(h, encoding='utf-8').read()
if HOOK_INCLUDE not in t:
    open(h, 'a', encoding='utf-8', newline='').write('\n/* GT6 port (tools/post_lift.py) */\n' + HOOK_INCLUDE + '\n')
    print('patched ppu_recomp.h')
for a, n in done.items():
    if n != 1:
        raise SystemExit(f'{a}: expected exactly one lifted body, found {n}')
if nomlaa_done != [1] * len(nomlaa_done):
    raise SystemExit(f'GT6_NO_MLAA: each of the four places must be found exactly once, found {nomlaa_done}')
if dof_diag_done != 1:
    raise SystemExit(f'GT6_DIAG_SKIP_DOF: expected exactly one command guard, found {dof_diag_done}')
if audio_flow_done != 1:
    raise SystemExit(f'GT6_AUDIO_FLOW: expected exactly one completion hook, found {audio_flow_done}')
if aspect_done != 1:
    raise SystemExit(f'GT6_ASPECT: expected exactly one aspect getter hook, found {aspect_done}')
if audio_fifo_done != 2:
    raise SystemExit(f'GT6_AUDIO_FIFO: expected exactly one begin/end pair, found {audio_fifo_done}')
if frame_wait_done != 2 or flip_target_done != 1 or frame_time_done != 1:
    raise SystemExit(f'GT6_FRAME_TIMING: expected two wait copies, one flip target and one time hook, '
                     f'found {frame_wait_done} / {flip_target_done} / {frame_time_done}')
if movie_post_done != 2:
    raise SystemExit(f'GT6_MOVIE_POST: expected one begin/end pair, found {movie_post_done}')
if movie_clock_done != 4:
    raise SystemExit(f'GT6_MOVIE_CLOCK: expected four sites, found {movie_clock_done}')
if bgm_trace_done != 6:
    raise SystemExit(f'GT6_BGM_TRACE: expected exactly six start hooks, found {bgm_trace_done}')
if bgm_file_done != 2:
    raise SystemExit(f'GT6_SGX_FILE_HANDLE: expected callback hook and branch, found {bgm_file_done}')
if spurs_done != 3:
    raise SystemExit(f'GT6_SPURS_SPUS: expected the request, the clamp and the job list hook once each, found {spurs_done}')
if trace - traced:
    raise SystemExit('not lifted function entries: ' + ', '.join(sorted(trace - traced)))
if tail_calls:
    print('WARNING: setjmp call sites that are not plain calls (the host setjmp frame would not survive):')
    for s in tail_calls:
        print('  ', s)
from trainer_post_lift import apply_trainer
apply_trainer(d)
print('post_lift: ok' + (f' (tracing {", ".join(sorted(traced))})' if traced else ''))
