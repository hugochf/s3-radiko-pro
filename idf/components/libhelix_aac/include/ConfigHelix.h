#pragma once
/*
 * Minimal plain-C config for the vendored Helix AAC decoder (replaces the
 * upstream C++/Arduino ConfigHelix.h). Enable SBR so HE-AAC decodes — Radiko
 * streams can be HE-AAC.
 */
#define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
