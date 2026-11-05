# Passive PGS identification

`CPGSProbe` observes packets already delivered by the playback demuxer. It never
seeks, opens extra media, changes selected tracks, rewrites MKV, or replaces the
rendered subtitle bitmaps. `GetSubtitleStreamInfo()` adds one generated runtime
description prefixed with ★; original selection names, languages, flags and
saved choices are not rewritten by label generation. Optional automatic selection
uses typed results separately. Explicit descriptive titles stay verbatim.

The script and bilingual rules originate from the 2026-11-05 mpv working-tree
snapshot (base 8e8f33b2dabcdee6579bffc5af4377539705d4b5 plus local changes).
Original files and hashes are retained in the R10 pgs-mpv-align evidence;
LGPL-2.1-or-later provenance is preserved. At the user's later request, all
styling detection was removed: no colour comparison, position/motion heuristic,
styling metadata interpretation or generated styled/suspected-styled labels.
A word such as 特效 in an original authoritative title is not erased.

Five consecutive script-specific characters confirm Simplified/Traditional
Chinese. Shared/ambiguous characters do not vote; confidence and bitmap/text
deduplication remain unchanged. A confidently recognized second physical line containing English letters and
only ASCII digits/punctuation or common typographic punctuation confirms English
immediately (at least two letters; every decoded character confidence >=70%).
The unfiltered line is checked so low-confidence Chinese cannot disappear and
produce false English evidence. Separate Chinese evidence (two Han characters
or known script) is still required. Script identification keeps its five-character
rule. Other layouts retain the fallback: at least four Han characters, eight
Latin letters and two Latin word groups in each of three usable captions. Script and bilingual completion are separate; a confirmed
bilingual track no longer needs extra samples for styling. A script-confirmed
track without bilingual evidence finishes after 24 distinct OCR captions;
the maximum remains 96 attempts/decoded captions and 4096 display sets.

Chinese aliases and unknown-language candidates retain the original filter.
A useful title, including 简英/繁英, skips bitmap analysis. Language tags alone
can seed script while leaving bilingual identification active. Track comments,
description and summary participate in the existing metadata input. Generated
labels include 简体中文, 繁体中文, 简英双语, 繁英双语, 简繁未定, 局部翻译 and
含强制字幕. The last label observes PGS object flags; the container's native
forced-track flag and the renderer's forced-object handling are not changed.

One low-priority coordinator uses a single model/session with two intra-op CPU
threads, sequential inter-op execution and spinning disabled. Each text line is scaled to32 pixels high, centred on white in the model's48-high
tensor; width remains proportional with minimum320. Available work first decodes
layouts cheaply: bilingual preferences prioritize multiline images, monolingual
preferences prioritize single-line images. Within that group, matching script
precedes unknown then other script; Chinese metadata precedes unknown language.
Tracks rotate within each level; each
track's PGS packet order is preserved. The bounded queue remains 1024 packets /
4 MiB including FFmpeg padding, with lower-priority backlog eviction and decoder
generation resets when higher-priority work needs space. Prepared grayscale images
hold at most one sample per track. Decode-ahead yields after64 packets or when
prepared images reach8 MiB (one final sample may exceed that threshold, by at most
four bounded2080x544 images). Epoch/generation changes discard prepared work too;
pending images protect tracks from premature sparse completion. No decoded user
images or recognized text are written to disk. Sample spacing is at
least one media second and the recent-ink hash history is 128 entries.
The per-image soft cutoff stays four seconds for multiline/slower ARM inputs.

## Sparse tracks and model lifetime

The player supplies actual advancing playback time, excluding pauses, buffering
and menus. Seek/discontinuity resets the activity window; demux read-ahead PTS
cannot prematurely age a track. Nonempty PGS PCS objects count as data activity;
clear/empty PCS packets do not. At a one-second maintenance cadence while a model
is loaded, a candidate is active only after a full 60 playback seconds of
observation, with at least three data events in the last 60 seconds and at least
one in the last 20 seconds. At least two active tracks and a strict majority of
registered candidates are required before sparsity can finish other tracks.

An unfinished track with no new visible bitmap candidate for 60 playback seconds
and no valid queued packets is then marked sampling-finished. This deadline is
separate from the peer-track PCS activity above: transparent objects, clear
commands and repeated ink do not renew it. Bitmap alpha/ink checks, sample
spacing and deduplication all run before renewing the deadline or invoking OCR.
Recognizability/confidence does not decide whether a visible candidate counts.
The track is not relabelled as
empty, ordinary, non-Chinese or non-forced: known script and forced evidence are
kept, and an unconfirmed script stays unknown. A later seek rearms tracks which
finished only because of sparsity; a later file open resets everything. Queued
work, a lone subtitle track, a newly seen track or a scene with no majority of
active subtitles cannot cause premature sparse completion.

Once all known candidates have finished, the worker destroys the shared session
and decoder state. Later packets of a completed track only update activity and
forced-object observations; they do not recreate the model. The coordinator then
blocks without a polling timer. New candidates may still be discovered from
later demux packets. Close cancels in-flight inference and joins the worker.
No recognized dialogue is persisted or logged; result/timing and completion
reasons are debug logged without text.

`subtitles.identifypgs` is default-on below preferred subtitle language in
Player / Language / Subtitles. Immediately below are default-on
`subtitles.pgsautoswitch` and `subtitles.pgspreferredtype` (0 Simplified, 1
Traditional, 2 Simplified/English default, 3 Traditional/English). Preference
also drives recognition priority while auto-switch is off. A monolingual match
requires authoritative metadata or completed sampling, not merely a lack of
English evidence so far. Exact preference wins over fallback, even if the current
track matches the fallback. After all candidates which could still become an
exact match have completed, Simplified and Simplified/English may fall back to
each other, likewise Traditional and Traditional/English. No cross-script
fallback is used. Candidate metadata is registered before packets arrive so a
late subtitle does not falsely imply absence. Authoritative titles are evaluated
without consuming probe slots; candidates beyond the probe capacity remain
unresolved for fallback rather than being silently declared absent. Automatic messages recheck the
same selection policy at execution, including fallback and pending candidates. With auto-switch enabled, the player switches once to
a matching non-forced PGS track; a matching current track is retained. Explicit
subtitle selection or visibility changes during this playback suppress switching,
and subtitles already hidden are not enabled. Automatic selection does not invoke
subtitle recall seeking or restart video. Turning identification off hides generated labels and cancels
queued analysis on the next PGS packet. Builds without ONNX Runtime retain
ordinary playback and hide this optional setting.

The separately packaged PP-OCRv5 mobile model is Apache-2.0; ONNX Runtime v1.23.2
is MIT licensed. Licenses, model hash and provenance are installed with it.
No runtime network service or target Python is required for identification.


## Saved selections and smooth switching

A per-file subtitle choice restored from the video database (or saved hidden
state) takes precedence over OCR auto-selection. The runtime provenance flag is
not itself persisted as a preference. Auto-selection also waits until the
asynchronous settings read completes.

Manual and automatic changes to Chinese/unknown main-demux PGS or text streams
replay bounded compressed subtitle history into the subtitle decoder only. PGS
history starts at an epoch boundary and retains the epoch needed at display
time, including clear compositions. Completed replay overlays are published as
a batch; the subtitle associations of already queued/displayed video buffers
are rebuilt at their own PTS, without discarding video frames. A generation
guard rejects concurrently collected old-track overlays. GPU resource cleanup
remains on the render thread. Audio and video are not sought or flushed. Other explicit languages and
menu paths retain their existing behavior. No extra file/network reader is used.

The cache is independent of OCR, capped at 16 MiB total, 4 MiB per track, 32
tracks and 1024 packets per track. It clears on seek, discontinuity, stream
replacement and playback end. Overflow discards a complete dependency chain.
If no valid cached epoch is available, switching waits for new subtitle data;
this does not promise instantaneous reconstruction of data never received.
