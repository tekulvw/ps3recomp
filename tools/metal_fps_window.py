#!/usr/bin/env python3
"""Request/collect one explicit Metal FPS wall window. No process or app control."""
import argparse,json,math,os,pathlib,tempfile,time
PREFIX='[RSX FPS window] '
COUNT_FIELDS=('admitted','committed','gpu_completed','gpu_failed','displayed',
    'positive_presentations','duplicate_presented_time','ambiguous_presented_time',
    'unique_guest_displayed','repeated_guest_displayed','uncorrelated','unresolved',
    'uncommitted_events','missing_presentations','zero_presented_time',
    'invalid_presented_time','duplicate_callbacks','out_of_order','overflow',
    'identity_errors','nil_drawables','skipped_blits','submission_failures',
    'guest_id_floor','preexisting_guest_submissions')
def finite_number(value):
    return type(value) in (int,float) and math.isfinite(value)

def collect(lines,request_id,requested_seconds=None):
    if requested_seconds is not None and (not finite_number(requested_seconds) or not 0<requested_seconds<=300):
        raise ValueError('Invalid requested duration')
    for line in lines:
        if not line.startswith(PREFIX):continue
        event=json.loads(line[len(PREFIX):])
        if event.get('request_id')!=request_id:continue
        if event.get('phase')=='rejected':raise RuntimeError(event.get('reason','rejected'))
        if event.get('phase')=='complete':
            for field in ('t0','t1','seconds','verified_guest_fps'):
                if not finite_number(event.get(field)):raise ValueError('Nonfinite or invalid '+field)
            for field in COUNT_FIELDS:
                if field in event and (type(event[field]) is not int or not 0<=event[field]<2**64):
                    raise ValueError('Invalid counter '+field)
            if 'unique_guest_displayed' not in event:raise ValueError('Missing guest count')
            for field in ('qualified','meets_30_displayed_guest_fps'):
                if type(event.get(field)) is not bool:raise ValueError('Invalid boolean '+field)
            elapsed=event['t1']-event['t0']
            clock_ulp=max(math.ulp(event['t0']),math.ulp(event['t1']))
            if event['t0']<=0 or not 0<event['seconds']<=300 or elapsed<=0 or abs(elapsed-event['seconds'])>1e-9+2*clock_ulp:
                raise ValueError('Invalid wall interval')
            if requested_seconds is not None and abs(event['seconds']-requested_seconds)>.5e-9+clock_ulp:
                raise ValueError('Wall duration differs from requested duration')
            expected=event['unique_guest_displayed']/event['seconds']
            if event['verified_guest_fps']<0 or not math.isfinite(expected) or abs(event['verified_guest_fps']-expected)>1e-6:
                raise ValueError('Inconsistent verified rate')
            accepted=event['qualified'] and expected>=30
            if requested_seconds is not None:accepted=accepted and event['unique_guest_displayed']/requested_seconds>=30
            if event['meets_30_displayed_guest_fps']!=accepted:
                raise ValueError('Inconsistent acceptance flag')
            event['meets_30_displayed_guest_fps']=accepted
            return event
    return None

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--control',type=pathlib.Path,required=True)
    p.add_argument('--log',type=pathlib.Path,required=True)
    p.add_argument('--request-id',type=int,required=True)
    p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--output',type=pathlib.Path,required=True)
    args=p.parse_args()
    if not 0<args.request_id<2**64 or not math.isfinite(args.seconds) or not 0<args.seconds<=300:p.error('ID must fit positive uint64 and seconds must be in (0,300]')
    requested_seconds=float(f'{args.seconds:.9f}')
    if requested_seconds<=0:p.error('Duration is below control-file precision')
    with args.log.open('r') as log:
        log.seek(0,os.SEEK_END)
        fd,temporary=tempfile.mkstemp(prefix=args.control.name+'.',dir=args.control.parent)
        try:
            with os.fdopen(fd,'w') as f:f.write(f'{args.request_id} {args.seconds:.9f}\n')
            os.replace(temporary,args.control)
        finally:
            if os.path.exists(temporary):os.unlink(temporary)
        deadline=time.monotonic()+args.seconds+20
        pending=''
        while time.monotonic()<deadline:
            pending+=log.read()
            end=pending.rfind('\n')
            if end>=0:
                result=collect(pending[:end+1].splitlines(),args.request_id,requested_seconds);pending=pending[end+1:]
                if result is not None:
                    args.output.write_text(json.dumps(result,indent=2)+'\n')
                    print(json.dumps(result));return
            time.sleep(.1)
    raise TimeoutError('No complete measurement; main queue may be stalled or FPS/window control disabled')
if __name__=='__main__':main()
