// Included by ReverseSamplerTests.cpp: exercises the real processor and DSP.
static void testNativeAttack()
{
    std::cout << "native transient fidelity\n";
    const auto file = tmp ("native_transient.wav");
    file.deleteFile();
    juce::AudioBuffer<float> source (2, 4800);
    source.clear();
    std::mt19937 rng (42);
    std::uniform_real_distribution<float> noise (-0.4f, 0.4f);
    for (int ch=0; ch<2; ++ch) {
        for (int n=0; n<4800; ++n) source.setSample (ch, n, noise (rng));
        source.setSample (ch, 0, ch == 0 ? 0.75f : -0.5f);
    }
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
        CHECK (writer != nullptr, "transient fixture writer");
        if (! writer) return;
        writer->writeFromAudioSampleBuffer (source, 0, 4800);
    }
    for (int mode=0; mode<2; ++mode) for (int reverse=0; reverse<2; ++reverse) {
        auto p = std::make_unique<ReverseSamplerProcessor>();
        p->setPlayConfigDetails (0, 2, rate, 512); p->prepareToPlay (rate, 512);
        CHECK (p->loadSample (0, file), "transient fixture loads");
        setParam (*p, L (0, ParamIDs::timeMode), (float)mode);
        setParam (*p, L (0, ParamIDs::reverse), (float)reverse);
        // Do not set Attack: new instances must preserve the transient by default.
        juce::AudioBuffer<float> block (2, 512); block.clear();
        juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        p->processBlock (block, midi);
        float error = 0;
        for (int ch=0; ch<2; ++ch) for (int n=0; n<512; ++n)
            error = juce::jmax (error, std::abs (block.getSample (ch,n)-source.getSample (ch,reverse ? 4799-n : n)));
        std::cout << "    mode " << mode << " reverse " << reverse << " first 512 error " << error << "\n";
        CHECK (error < 2.0e-6f, "native attack preserved from first sample, stereo/forward/reverse/both modes");
        setParam (*p, ParamIDs::attack, 5);
        juce::MemoryBlock state; p->getStateInformation (state);
        ReverseSamplerProcessor restored; restored.setStateInformation (state.getData(), (int)state.getSize());
        CHECK (std::abs (restored.getValueTreeState().getRawParameterValue (ParamIDs::attack)->load()-5.0f)<1.0e-5f,
               "saved positive Attack is not overridden by new default");
        restored.waitForPendingLoads (30000);
    }
}

static void testAudioQuality()
{
    std::cout << "delay audio quality\n";
    auto p = std::make_unique<ReverseSamplerProcessor>();
    rs::FxRackDSP rack; rack.connect (p->getValueTreeState());
    auto set = [&] (const char* id, float v) { setParam (*p, rs::fxID (0,id), v); };
    set ("type",1); set ("a",1); set ("b",0); set ("mix",100); set ("p1",0); set ("p2",0);
    set ("delayStyle",0);
    // A flat source exposes holes at reverse splice boundaries. The previous
    // implementation faded both sides to zero at every 40 ms boundary.
    rack.prepare (rate);
    juce::AudioBuffer<float> constant (4,24000); constant.clear();
    for (int n=0; n<24000; ++n) constant.setSample (0,n,0.2f);
    rack.process (constant,24000);
    float worst = 0;
    for (int n=6000; n<24000; ++n) worst = juce::jmax (worst,std::abs (constant.getSample (0,n)-0.2f));
    CHECK (worst < 1.0e-6f,"reverse overlap has no periodic level holes");

    // At fixed settings diffusion is an allpass, not a dry/allpass comb mix:
    // impulse energy must be preserved at intermediate amounts as well as 100%.
    for (float density : { 10.0f, 50.0f, 100.0f }) {
        set ("diffusion",density); rack.prepare (rate);
        juce::AudioBuffer<float> impulse (4,96000); impulse.clear(); impulse.setSample (0,400,0.5f);
        rack.process (impulse,96000);
        double energy = 0; float maxSample = 0;
        for (int n=0; n<96000; ++n) { const float v=impulse.getSample (0,n); energy+=(double)v*v; maxSample=juce::jmax(maxSample,std::abs(v)); }
        std::cout << "    diffusion " << density << " energy=" << energy << " peak=" << maxSample << "\n";
        CHECK (std::abs (energy-0.25)<0.001,"diffusion preserves impulse energy at all densities");
        CHECK (impulse.getMagnitude (2,0,96000)==0.0f,"diffusion cannot leak into unrouted layer");
    }
    // The same sample stream must produce identical output at different host
    // block sizes. Includes all four delay styles, feedback and diffusion.
    for (double sr : { 44100.0, 48000.0, 96000.0 }) for (int style=0; style<4; ++style) {
        set ("delayStyle",(float)style); set ("p2",85); set ("diffusion",65); set ("amSplice",60);
        const int length=(int)sr;
        auto renderBlocks = [&] (int bs) {
            rack.prepare (sr); std::vector<float> output ((size_t)length);
            juce::AudioBuffer<float> block (4,bs);
            for (int pos=0; pos<length; pos+=bs) {
                block.clear(); const int count=juce::jmin(bs,length-pos);
                for (int n=0; n<count; ++n) {
                    float x=(pos+n)<length/2 ? 0.2f*(float)std::sin(juce::MathConstants<double>::twoPi*997.0*(pos+n)/sr) : 0.0f;
                    block.setSample(0,n,x); block.setSample(1,n,x);
                }
                rack.process(block,count);
                for (int n=0; n<count; ++n) output[(size_t)(pos+n)]=block.getSample(0,n);
            }
            return output;
        };
        auto a=renderBlocks(64), b=renderBlocks(511);
        float error=0; for (int n=0;n<length;++n) error=juce::jmax(error,std::abs(a[(size_t)n]-b[(size_t)n]));
        CHECK (allFinite(a) && error<1.0e-6f,"delay is finite and block-size invariant at 44.1/48/96k");
    }
    // Feedback automation on a stationary signal must ramp rather than jump.
    set ("delayStyle",2); set ("replica",1); set ("diffusion",0); set ("p2",0); rack.prepare(rate);
    juce::AudioBuffer<float> steady (4,24000); steady.clear();
    for(int n=0;n<24000;++n) steady.setSample(0,n,0.2f);
    rack.process(steady,24000); set("p2",100);
    steady.clear(); for(int n=0;n<24000;++n) steady.setSample(0,n,0.2f);
    rack.process(steady,24000);
    float step=0;
    for(int n=1;n<24000;++n) step=juce::jmax(step,std::abs(steady.getSample(0,n)-steady.getSample(0,n-1)));
    CHECK (step<0.001f,"feedback automation is smoothed");
    // Changing a sync division must not change a steady level or erase history.
    set("sync",1); set("division",10); set("p2",0); rack.prepare(rate);
    juce::AudioBuffer<float> automation (4,48000); automation.clear();
    for(int n=0;n<48000;++n) automation.setSample(0,n,0.2f);
    rack.process(automation,48000);
    set("division",8);
    automation.clear(); for(int n=0;n<48000;++n) automation.setSample(0,n,0.2f);
    rack.process(automation,48000);
    float divisionError=0;
    for(int n=0;n<48000;++n) divisionError=juce::jmax(divisionError,std::abs(automation.getSample(0,n)-0.2f));
    CHECK(divisionError<1.0e-6f,"Replica division changes crossfade without gaps or gain jumps");
    // 0.16: a reset request fades the old sound out over 20 ms instead of cutting
    // it, then clears the memory: nothing stale may sound after the fade.
    float resetPrevious=automation.getSample(0,47999), resetStep=0;
    rack.requestReset(0); automation.clear(); rack.process(automation,48000);
    for(int n=0;n<48000;++n) { resetStep=juce::jmax(resetStep,std::abs(automation.getSample(0,n)-resetPrevious)); resetPrevious=automation.getSample(0,n); }
    const int resetFade=(int)(0.025*rate);
    CHECK(automation.getMagnitude(0,resetFade,48000-resetFade)==0.0f,"explicit rack reset cannot read stale memory after its fade");
    CHECK(resetStep<0.01f,"a reset request fades out instead of cutting");
    // An Off high cut must be truly transparent even below a 40 kHz host rate.
    for(double sr : {32000.0,44100.0,48000.0,96000.0}) {
        std::atomic<float> gain{0}, pan{0}, width{100}, low{20}, high{20000};
        rs::LayerBus bus; bus.setParameters({&gain,&pan,&width,&low,&high}); bus.prepare(sr,512);
        float left[512]{},right[512]{}; left[0]=right[0]=0.5f;
        bus.process(left,right,512);
        float err=std::abs(left[0]-0.5f);
        for(int n=1;n<512;++n) err=juce::jmax(err,std::abs(left[n]));
        CHECK(err<1.0e-7f,"default layer bus is transparent with High Cut Off at all host rates");
    }
    // Measure folded third-harmonic energy against the former point-sampled
    // saturator through the same tone filter, not against a mocked new algorithm.
    set("type",3); set("p1",100); set("p2",100); set("p3",50); rack.prepare(rate);
    juce::AudioBuffer<float> driven(4,48000); driven.clear();
    std::vector<float> reference(48000);
    const float toneAlpha=1.0f-std::exp(-juce::MathConstants<float>::twoPi*18000.0f/(float)rate);
    float filtered=0;
    for(int n=0;n<48000;++n) {
        const float x=0.6f*(float)std::sin(juce::MathConstants<double>::twoPi*9000.0*n/rate);
        driven.setSample(0,n,x);
        filtered+=toneAlpha*(std::tanh(20.0f*x)/std::tanh(20.0f)-filtered);
        reference[(size_t)n]=filtered;
    }
    rack.process(driven,48000);
    std::vector<float> actual(driven.getReadPointer(0),driven.getReadPointer(0)+48000);
    // The drive is loudness-compensated: at full drive it is far quieter than the raw tanh, so
    // compare levels relative to its own fundamental. Folded energy is measured against the former
    // tanh, the 9 kHz fundamental against the same drive at 1 kHz.
    rack.prepare(rate);
    juce::AudioBuffer<float> lowDriven(4,48000); lowDriven.clear();
    for(int n=0;n<48000;++n) lowDriven.setSample(0,n,0.6f*(float)std::sin(juce::MathConstants<double>::twoPi*1000.0*n/rate));
    rack.process(lowDriven,48000);
    std::vector<float> lowActual(lowDriven.getReadPointer(0),lowDriven.getReadPointer(0)+48000);
    const double oldAlias=level(reference,21000.0,0.5,0.2), newAlias=level(actual,21000.0,0.5,0.2);
    const double oldFundamental=level(reference,9000.0,0.5,0.2), newFundamental=level(actual,9000.0,0.5,0.2), lowFundamental=level(lowActual,1000.0,0.5,0.2);
    std::cout<<"    drive folded third harmonic "<<oldAlias<<" -> "<<newAlias<<", fundamental "<<oldFundamental<<" -> "<<newFundamental<<" (same drive at 1 kHz: "<<lowFundamental<<")\n";
    CHECK(newAlias/newFundamental<oldAlias/oldFundamental*0.85,"ADAA reduces folded drive harmonics relative to former tanh");
    CHECK(newFundamental>lowFundamental*0.6,"alias reduction retains the driven fundamental");
    // Reverse time automation must continue the actual outgoing grain, not
    // accidentally jump to a history address computed from the new length.
    set("type",1); set("delayStyle",0); set("diffusion",0); set("sync",0);
    set("p1",0); set("p2",0); rack.prepare(rate);
    juce::AudioBuffer<float> beforeTime(4,9600); beforeTime.clear();
    const auto wave=[](int n) { return 0.2f*(float)std::sin(juce::MathConstants<double>::twoPi*997.0*n/rate); };
    for(int n=0;n<9600;++n) beforeTime.setSample(0,n,wave(n));
    rack.process(beforeTime,9600); // exactly five 40 ms grains
    set("p1",100);
    juce::AudioBuffer<float> afterTime(4,256); afterTime.clear();
    for(int n=0;n<256;++n) afterTime.setSample(0,n,wave(9600+n));
    rack.process(afterTime,256);
    const float outgoing=wave(9600-1-2*1920);
    CHECK(std::abs(afterTime.getSample(0,0)-outgoing)<1.0e-6f,
          "Reverse time change continues old outgoing head at splice boundary");
    // The obsolete host parameter ignores setValue, saved state and slot swaps.
    set("reflections",8);
    CHECK (p->getValueTreeState().getRawParameterValue(rs::fxID(0,"reflections"))->load()==1.0f,"legacy automation stays at one");
    auto legacy=p->getValueTreeState().copyState();
    for(int n=0;n<legacy.getNumChildren();++n) {
        auto child=legacy.getChild(n);
        if(child.getProperty("id").toString()==rs::fxID(0,"reflections")) child.setProperty("value",8,nullptr);
    }
    juce::MemoryBlock data; juce::AudioProcessor::copyXmlToBinary(*legacy.createXml(),data);
    p->setStateInformation(data.getData(),(int)data.getSize());
    CHECK (p->getValueTreeState().getRawParameterValue(rs::fxID(0,"reflections"))->load()==1.0f,"old saved count restores as one");
    p->swapFxSlots(0,1);
    CHECK (p->getValueTreeState().getRawParameterValue(rs::fxID(1,"reflections"))->load()==1.0f,"slot swap preserves fixed one");
}
