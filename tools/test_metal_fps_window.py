"""Collector tests catch endpoint denominator substitutions and truncated lines."""
import json,pathlib,subprocess,sys,tempfile,time,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from metal_fps_window import collect,PREFIX
class WindowCollectorTests(unittest.TestCase):
    def event(self,**changes):
        event=dict(phase='complete',request_id=7,t0=10,t1=70,seconds=60,
                   unique_guest_displayed=120,verified_guest_fps=2,
                   qualified=True,meets_30_displayed_guest_fps=False)
        event.update(changes);return PREFIX+json.dumps(event)
    def test_exact_wall_interval_includes_stalls(self):
        self.assertEqual(collect([self.event()],7)['verified_guest_fps'],2)
    def test_rejects_endpoint_denominator(self):
        with self.assertRaises(ValueError):collect([self.event(verified_guest_fps=30)],7)
    def test_rejects_inconsistent_wall_interval(self):
        with self.assertRaises(ValueError):collect([self.event(seconds=4)],7)
    def test_unknown_requests_do_not_finish(self):self.assertIsNone(collect([self.event()],8))
    def test_busy_is_an_error(self):
        with self.assertRaises(RuntimeError):collect([self.event(phase='rejected',reason='busy')],7)
    def test_rejects_nonfinite_wall_values_and_rates(self):
        for field in ('t0','t1','seconds','verified_guest_fps'):
            for value in (float('nan'),float('inf'),-float('inf')):
                with self.subTest(field=field,value=value),self.assertRaises(ValueError):
                    collect([self.event(**{field:value})],7)
    def test_rejects_negative_or_noninteger_counts(self):
        for field in ('unique_guest_displayed','committed','nil_drawables','guest_id_floor'):
            for value in (-1,1.5,True,2**64):
                with self.subTest(field=field,value=value),self.assertRaises(ValueError):
                    collect([self.event(**{field:value})],7)
    def test_checks_requested_duration_not_only_internal_consistency(self):
        with self.assertRaises(ValueError):
            collect([self.event(t1=14,seconds=4,verified_guest_fps=30)],7,60)
    def test_exact_requested_duration_accepts_logged_clock_rounding(self):
        event=self.event(t0=660135.670899333,t1=660195.670899333)
        self.assertEqual(collect([event],7,60)['verified_guest_fps'],2)
    def test_rejects_shortened_request_near_thirty_fps_boundary(self):
        # 30 frames / 1.0000001 seconds is below 30; silently accepting a
        # one-second result would incorrectly accept that requested window.
        with self.assertRaises(ValueError):
            collect([self.event(t1=11,seconds=1,unique_guest_displayed=30,
                verified_guest_fps=30,meets_30_displayed_guest_fps=True)],7,1.0000001)
    def test_rejects_inconsistent_acceptance_flags(self):
        for fields in (dict(qualified=False,meets_30_displayed_guest_fps=True),
                       dict(unique_guest_displayed=1800,verified_guest_fps=30,
                            meets_30_displayed_guest_fps=False)):
            with self.subTest(fields=fields),self.assertRaises(ValueError):
                collect([self.event(**fields)],7)
    def test_recomputes_acceptance_from_unrounded_rate(self):
        # The printed six-decimal rate rounds to 30, but 1799 / 59.966666667
        # is below 30. A rounded log value must not change the threshold.
        event=self.event(t1=69.966666667,seconds=59.966666667,
                         unique_guest_displayed=1799,verified_guest_fps=30)
        self.assertFalse(collect([event],7)['meets_30_displayed_guest_fps'])
        self.assertTrue(collect([self.event(unique_guest_displayed=1800,
            verified_guest_fps=30,meets_30_displayed_guest_fps=True)],7)['meets_30_displayed_guest_fps'])
    def test_rejects_nonboolean_qualification(self):
        for field in ('qualified','meets_30_displayed_guest_fps'):
            with self.subTest(field=field),self.assertRaises(ValueError):
                collect([self.event(**{field:1})],7)
    def test_cli_enforces_the_duration_it_requested(self):
        with tempfile.TemporaryDirectory(prefix='fps-collector-fixture-') as tmp:
            root=pathlib.Path(tmp);log=root/'log';log.write_text('')
            process=subprocess.Popen([sys.executable,str(pathlib.Path(__file__).with_name('metal_fps_window.py')),
                '--control',str(root/'control'),'--log',str(log),'--request-id','7',
                '--seconds','1','--output',str(root/'result')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                deadline=time.monotonic()+5
                while not (root/'control').exists() and process.poll() is None and time.monotonic()<deadline:
                    time.sleep(.01)
                self.assertTrue((root/'control').exists())
                with log.open('a') as output:
                    output.write(self.event(t1=10.5,seconds=.5,verified_guest_fps=240,
                        meets_30_displayed_guest_fps=True)+'\n')
                stdout,stderr=process.communicate(timeout=5)
                self.assertNotEqual(process.returncode,0,(stdout,stderr))
                self.assertFalse((root/'result').exists())
            finally:
                if process.poll() is None:process.kill();process.communicate()
if __name__=='__main__':unittest.main()
