# Avisynth 32-bit proxy

Embed 32-bit Avisynth 2.6 or Avisynth+ environment within 64-bit VapourSynth.

    avsw.Eval(string script, clip[] "clips", string[] "clip_names", string "avisynth", string "slave", string "slave_log")
    
 * **script** - Avisynth script fragment
 * **clips** - VapourSynth clips ("nodes") to inject into Avisynth environment
 * **clip_names** - Avisynth variable name corresponding to injected clip
 * **avisynth** - Path to Avisynth DLL. The default uses the process DLL search path.
 * **slave** - Path to avshost_native.exe slave process. The plugin path is searched by default.
 * **slave_log** - Log file for slave process.
 
The function returns the result of the Avisynth script, which may be an integer, float, string, or clip. If the result is a clip, the name of the return value is "clip", otherwise it is "result".

## Examples
    import vapoursynth as vs
    
    core = vs.get_core()
    
    red = core.std.BlankClip(color=[255, 0, 0])
    green = core.std.BlankClip(color=[0, 255, 0])
    # Before executing the Avisynth script, "r" and "g" are set to the bound clips.
    c = core.avsw.Eval("Merge(r, g)", clips=[red, green], clip_names=["r", "g"])
    c.set_output()
