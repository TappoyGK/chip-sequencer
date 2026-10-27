"""Expression priority, silent echo validation, and repeatable compile timings."""
import copy,json,math,pathlib,statistics,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
probe=pathlib.Path(sys.argv[1]).resolve()
base=json.loads((root/'examples/performance.json').read_text())
base.update(notes=[],sound_changes=[],dynamics=[],hairpins=[],echoes=[],tempo_ramps=[],
            tempos=[{'tick':0,'bpm':120}],loop=None,
            order=[{'id':'whole','from_tick':0,'to_tick':1536}])
def note(voice='voice0',at=0,end=96):
    return dict(id='note',source_id=voice,at=at,end=end,pitch=60,volume=8,
                accent=False,staccato=False,tom=False,drum='none',grace_pitches=[])
with tempfile.TemporaryDirectory(prefix='chip-controls-') as temp:
    src=pathlib.Path(temp)/'input.json';out=pathlib.Path(temp)/'output.json'
    def run(data,accepted=True):
        src.write_text(json.dumps(data))
        r=json.loads(subprocess.check_output([str(probe),str(src),str(out)],text=True))
        assert bool(r['accepted'])==accepted,r
        assert r['result']==(0 if accepted else 1),r
        return r,json.loads(out.read_text()) if accepted else None
    echo=dict(id='e1',source_id='voice0',destination_id='voice1',from_tick=0,
              to_tick=1000,delay_ticks=48,gain=.35)
    cases=0
    for notes in [[],[note('voice1')],[note()]]:
        for echoes in [[echo,dict(echo,id='e2',from_tick=100,to_tick=1200)],
                       [dict(echo,delay_ticks=100000000)],
                       [dict(echo,delay_ticks=5761)]]:
            x=copy.deepcopy(base);x.update(notes=notes,echoes=echoes);run(x,False);cases+=1
        for echoes in [[echo], [dict(echo,delay_ticks=5760)],
                       [dict(echo,id='e2',from_tick=1000,to_tick=1536),echo],
                       [echo,dict(echo,id='e2',source_id='voice1',destination_id='voice0')]]:
            x=copy.deepcopy(base);x.update(notes=notes,echoes=echoes);run(x);cases+=1
        # A delay exceeding the bound only inside a ramp window must be rejected.
        x=copy.deepcopy(base);x.update(notes=notes,echoes=[dict(echo,delay_ticks=96,to_tick=700)],
            tempos=[dict(tick=0,bpm=120),dict(tick=384,bpm=.005),dict(tick=768,bpm=120)],
            tempo_ramps=[dict(from_tick=0,to_tick=384),dict(from_tick=384,to_tick=768)])
        run(x,False);cases+=1
        x=copy.deepcopy(base);x.update(notes=notes,echoes=[dict(echo,delay_ticks=1)],tempos=[dict(tick=0,bpm=100000)])
        run(x,False);cases+=1
    print(f'echo validation: {cases} silent/other-source/active-source and boundary cases: PASS')
    x=copy.deepcopy(base);x['notes']=[note(end=1536)]
    x['dynamics']=[dict(id=f'd{i}',source_id=voice,tick=t,volume=v)
        for i,(voice,t,v) in enumerate([('voice0',768,3),('voice0',0,5),('voice0',192,12),('voice0',192,2),('voice1',0,15)])]
    x['hairpins']=[dict(id=f'h{i}',source_id='voice0',from_tick=a,to_tick=b,from_volume=f,to_volume=t)
        for i,(a,b,f,t) in enumerate([(96,384,4,12),(192,576,8,2),(192,480,6,14)])]
    for sound in x['sounds']:sound.update(attack_seconds=0,decay_seconds=0,release_seconds=0,sustain_level=1)
    _,seq=run(x)
    def volume(tick):
        value=8;last=-1
        for h in x['hairpins']:
            if h['from_tick']<=tick and h['from_tick']>=last:
                last=h['from_tick'];value=h['from_volume']+min(1,(tick-last)/(h['to_tick']-last))*(h['to_volume']-h['from_volume'])
        for d in x['dynamics']:
            if d['source_id']=='voice0' and last<d['tick']<=tick:last=d['tick'];value=d['volume']
        return math.floor(value+.5)
    for e in seq['events']:
        if e['kind']=='segment':assert e['volume']==volume(math.floor(e['sample']*120*96/(60*44100)+1e-7)),e
    print('expression priority: unsorted controls, equal-time ties, overlap, held endpoint, other voice: PASS')
    # Report medians; no wall-clock threshold in CI. The review evidence records
    # measured scaling, while correctness assertions stay deterministic.
    for n in [250,500,1000,2000]:
        x=copy.deepcopy(base);x['end_tick']=n*2;x['order'][0]['to_tick']=n*2
        x['notes']=[dict(note(at=i*2,end=i*2+1),id=f'n{i}') for i in range(n)]
        x['dynamics']=[dict(id=f'd{i}',source_id='voice0',tick=i*2,volume=8) for i in range(n)]
        values=[run(x)[0]['cpu_seconds'] for _ in range(5)]
        print(json.dumps(dict(notes=n,dynamics=n,cpu_seconds_median=statistics.median(values))))
