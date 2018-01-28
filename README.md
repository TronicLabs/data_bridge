This is a VST2.4 plugin that can be used to exchange 1..16 stereo channels between different processes / VST hosts.

It has two parameters:
- [0] "b_input": 0=copy VST input to data_bridge output, 1=copy data_bridge input to VST output
- [1] "channel_idx": 0..15 (selects data_bridge (stereo) channel)

The plugin also supports a proprietary effect opcode extension 'effDataBridgeGetNumChunksAvail' that can be used by custom VST hosts to query the current stream buffer load.

I'm using this plugin to send audio from my custom VST host to Propellerhead Reason.

Here's a demo video that shows the data_bridge plugin in action: https://vimeo.com/253055763

The plugin works very well with low ASIO buffer sizes (e.g. 256 samples) but for the demo video I had to select the "MME" output device in Reason, with an output buffer size of 4096 samples - otherwise the screencapture software wouldn't record the audio.

The source code is based on Pongasoft's "Hello World" VST and adds a lot of code missing in the original version, e.g. MIDI and SysEx handling, pause/resume, post-init via effOpen, patch query/restore, multi-threaded rendering, ... and it's still a single-file source ("plugin.cpp").

You can either use it for the intended purpose, or as a template for your own plugin coding experiments.

p.s.: There are MacOSX/Linux codepaths (mutex, shared memory, ..) in the source but these are completely untested (and may not even compile). Shouldn't be too difficult to get them working, though.
