"""Check diagnostics against independently known JSON paths and source positions."""
import copy,json,pathlib,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];probe=str(pathlib.Path(sys.argv[1]).resolve())
models={k:json.loads((root/'examples'/name).read_text()) for k,name in [('model','performance.json'),('sequence','sequence.json')]}
count=0
with tempfile.TemporaryDirectory(prefix='chip-diagnostics-') as temp:
    path=pathlib.Path(temp)/'input.json'
    def run(kind,value,pointer=None,reason=None,location=None,limit=None):
        global count
        text=value if isinstance(value,str) else json.dumps(value,indent=2)
        path.write_bytes(text.encode());args=[probe,kind,str(path)]+([limit] if limit else [])
        d=json.loads(subprocess.check_output(args,text=True).split("\ninstrumented library allocations:",1)[0]);count+=1
        if pointer is None:assert d['accepted']==1,d;return d
        assert d['accepted']==0 and d['path']==pointer,(pointer,d)
        assert reason.lower() in d['message'].lower(),d
        if location is not None:
            at=text.index(location);line=text[:at].count('\n')+1;column=at-text.rfind('\n',0,at)
            assert (d['offset'],d['line'],d['column'])==(at,line,column),d
        return d
    for kind,base in models.items():
        run(kind,base)
        for key,value,reason in [('version',2,'integer'),('song_id','bad id','identifier'),('sources',{},'array')]:
            x=copy.deepcopy(base);x[key]=value;run(kind,x,'/'+key,reason)
        x=copy.deepcopy(base);del x['sources'][2]['gain'];run(kind,x,'/sources/2/gain','missing')
        x=copy.deepcopy(base);x['sources'][2]['gain']=3.14159;run(kind,x,'/sources/2/gain','finite',location='3.14159')
        x=copy.deepcopy(base);x['sources'][1]['initial_sound']='missing';run(kind,x,'/sources/1/initial_sound','reference')
        x=copy.deepcopy(base);x['sounds'][0]['duty']=.4;run(kind,x,'/sounds/0/duty','duty')
        x=copy.deepcopy(base);x['unexpected/a~b']=True;run(kind,x,'/unexpected~1a~0b','unknown')
        text=json.dumps(base,indent=2).replace('"version": 1,','"version": 1,\n  "version": 1,')
        run(kind,text,'/version','duplicate')
        text=json.dumps(base,indent=2).replace('"version": 1','"version": 1.0')
        run(kind,text,'/version','fraction',location='1.0')
        run(kind,'{\n "x": @\n}', '', 'JSON value', location='@')
        run(kind,'{\n "x" 1\n}', '', "':'", location='1')
        run(kind,'{} trailing', '', 'after JSON', location='trailing')
        run(kind,'', '', 'document')
        run(kind,'{"x":"\\q"}', '', 'escape', location='q')
        d=run(kind,base,'','budget',limit='bytes');assert d['code']==3 and d['line']==0
        d=run(kind,base,'','budget',limit='memory');assert d['code']==3 and d['line']==0
        run(kind,base) # success clears a caller-owned buffer initialized to nonzero
    for kind,base in models.items():
        x=copy.deepcopy(base);x['sources'][0]['wave']='bogus';run(kind,x,'/sources/0/wave','wave')
        x=copy.deepcopy(base);x['sources'][1]['voice']=0;run(kind,x,'/sources/1/voice','duplicate')
        x=copy.deepcopy(base);x['version']=9223372036854775808;run(kind,x,'/version','64-bit')
        x=copy.deepcopy(base);x['sounds'][0]['tom_enabled']=0;run(kind,x,'/sounds/0/tom_enabled','boolean')
        run(kind,'{\r\n "x": @}', '', 'JSON value')
    x=copy.deepcopy(models['model']);x['order']=[];run('model',x,'/order','nonempty')
    x=copy.deepcopy(models['model']);x['notes'][0]['pitch']=999
    run('model',x,'/notes/0/pitch','integer',location='999')
    x=copy.deepcopy(models['model']);x['notes'][0]['source_id']='missing'
    run('model',x,'/notes/0/source_id','source')
    x=copy.deepcopy(models['model']);x['notes'][1]=dict(x['notes'][0],id='overlap')
    run('model',x,'/notes/1/at','overlap')
    x=copy.deepcopy(models['model']);x['notes'][1]['id']=x['notes'][0]['id']
    run('model',x,'/notes/1/id','duplicate')
    x=copy.deepcopy(models['model']);x['notes']=[];x['echoes']=[dict(id='e1',source_id='voice0',destination_id='voice1',from_tick=0,to_tick=1000,delay_ticks=1000000,gain=.3)]
    run('model',x,'/echoes/0/delay_ticks','delay')
    x['echoes'][0]['delay_ticks']=48;x['echoes'].append(dict(x['echoes'][0],id='e2',from_tick=100))
    run('model',x,'/echoes/1/from_tick','overlap')
    x=copy.deepcopy(models['sequence']);x['events'][0]['kind']='bogus'
    run('sequence',x,'/events/0/kind','kind')
    x=copy.deepcopy(models['sequence']);x['once_samples']+=1
    run('sequence',x,'/once_samples','sum')
print(f'diagnostics: {count} syntax/shape/range/reference/overlap/budget/path/reset checks: PASS')
