"""MCP contract checks; does not create a task or inject input."""
import json, subprocess, sys, unittest
from pathlib import Path

EXE = sys.argv.pop(1)

class Protocol(unittest.TestCase):
    def setUp(self):
        self.process = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding='utf-8')
        self.sequence = 0
        self.call('initialize', {'protocolVersion':'2025-11-25','capabilities':{},
                                'clientInfo':{'name':'test','version':'1'}})
        self.send({'jsonrpc':'2.0','method':'notifications/initialized'})

    def tearDown(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)
        self.assertEqual(self.process.returncode, 0, self.process.stderr.read())
        self.process.stdout.close()
        self.process.stderr.close()

    def send(self, message):
        self.process.stdin.write(json.dumps(message, ensure_ascii=False) + '\n')
        self.process.stdin.flush()

    def call(self, method, params=None):
        self.sequence += 1
        self.send({'jsonrpc':'2.0','id':self.sequence,'method':method,'params':params or {}})
        response = json.loads(self.process.stdout.readline())
        self.assertEqual(response['id'], self.sequence)
        return response

    def tool(self, name, args=None):
        return self.call('tools/call', {'name':name,'arguments':args or {}})['result']

    def test_catalog_and_contract(self):
        # Check advertised tool contracts and inactive/unapproved startup; self is this test case.
        tools = self.call('tools/list')['result']['tools']
        self.assertEqual(len(tools), 18)
        self.assertEqual(len({t['name'] for t in tools}), 18)
        for tool in tools:
            self.assertFalse(tool['inputSchema']['additionalProperties'])
        diagnostic = self.tool('diagnostics')
        self.assertFalse(diagnostic['isError'])
        self.assertFalse(diagnostic['structuredContent']['state']['active'])
        self.assertFalse(diagnostic['structuredContent']['state']['session_authorized'])
        begin = next(t for t in tools if t['name'] == 'begin_task')
        self.assertEqual(begin['inputSchema']['properties']['retry_consent']['type'], 'boolean')
        self.assertEqual(begin['inputSchema']['required'], ['title'])

    def test_requires_task_before_observation(self):
        result = self.tool('observe', {'window_id':'untrusted'})
        self.assertTrue(result['isError'])
        self.assertEqual(result['structuredContent']['error']['code'], 'TASK_REQUIRED')

    def test_foreground_host_contract(self):
        # Check foreground identity and activation schemas; self owns the idle MCP process.
        info = self.tool('diagnostics')['structuredContent']['foreground_permission']
        self.assertEqual(info['server_pid'], self.process.pid)
        self.assertIsInstance(info['server_process_created'], str)
        self.assertGreater(int(info['server_process_created']), 0)
        self.assertEqual(info['method'], 'AllowSetForegroundWindow')
        self.assertEqual(info['prompt_timeout_ms'], 15000)
        tools = self.call('tools/list')['result']['tools']
        activate = next(t for t in tools if t['name'] == 'activate_window')
        self.assertEqual(activate['inputSchema']['properties']['foreground_prompt']['type'], 'boolean')
        self.assertEqual(activate['inputSchema']['properties']['cursor_fallback']['type'], 'boolean')
        for key in ('x', 'y', 'observation_id'):
            self.assertIn(key, activate['inputSchema']['properties'])
        result = self.tool('activate_window', {'window_id':'x','foreground_prompt':False})
        self.assertEqual(result['structuredContent']['error']['code'], 'TASK_REQUIRED')
        helper = Path(EXE).with_name('desktop-control-foreground.exe')
        denied = subprocess.run([str(helper),'--grant',str(self.process.pid),str(int(info['server_process_created'])+1)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(denied.returncode, 1)
        self.assertFalse(json.loads(denied.stdout)['granted'])
        self.assertEqual(json.loads(denied.stdout)['win32_error'], 87)
        invalid = subprocess.run([str(helper),'--grant','4294967295',str(info['server_process_created'])],
                                 capture_output=True, text=True, timeout=5)
        self.assertEqual(json.loads(invalid.stdout)['win32_error'], 87)

    def test_invalid_arguments_are_rejected(self):
        for name,args in [('begin_task',{}),('begin_task',{'title':12}),
                          ('begin_task',{'title':'test','retry_consent':'true'}),
                          ('diagnostics',{'extra':True}),
                          ('report_agent_status',{'task_id':'x','phase':'invented'})]:
            result = self.tool(name,args)
            self.assertTrue(result['isError'])
            self.assertEqual(result['structuredContent']['error']['code'], 'INVALID_ARGUMENT')

    def test_invalid_title_has_no_side_effect(self):
        result = self.tool('begin_task', {'title':'two\nlines'})
        self.assertTrue(result['isError'])
        self.assertFalse(self.tool('diagnostics')['structuredContent']['state']['active'])

    def test_jsonrpc_errors_and_ping(self):
        self.assertEqual(self.call('ping')['result'], {})
        self.assertEqual(self.call('unrecognized')['error']['code'], -32601)
        self.process.stdin.write('{broken\n')
        self.process.stdin.flush()
        self.assertEqual(json.loads(self.process.stdout.readline())['error']['code'], -32700)
        self.assertFalse(self.tool('diagnostics')['isError'])

    def test_malformed_envelopes_do_not_crash(self):
        for message in [
            {'jsonrpc':2,'id':77,'method':'ping'},
            {'jsonrpc':'2.0','id':77,'method':'tools/call','params':[]},
            {'jsonrpc':'2.0','id':77,'method':'tools/call','params':{'name':42}},
        ]:
            self.send(message)
            response=json.loads(self.process.stdout.readline())
            self.assertTrue('error' in response or response['result']['isError'])
            self.assertFalse(self.tool('diagnostics')['isError'])

    def test_stop_and_resume_without_task(self):
        self.assertFalse(self.tool('stop')['structuredContent']['paused'])
        self.assertEqual(self.tool('resume')['structuredContent']['error']['code'], 'TASK_REQUIRED')


    def test_cli_timeout_diagnostics_and_validation(self):
        # Check combined flags, defaults and rejected values; self owns the idle MCP client.
        result = subprocess.run([EXE, '--timeout', '300', '--language', 'ru', '--diagnostics'],
                                capture_output=True, text=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        diagnostic = json.loads(result.stdout)
        self.assertEqual(diagnostic['state']['language'], 'ru')
        self.assertEqual(diagnostic['version'], '0.1.3')
        self.assertTrue(all(value == 300000 for value in diagnostic['timeouts'].values()))
        baseline = self.tool('diagnostics')['structuredContent']['timeouts']
        self.assertIsNone(baseline['override_ms'])
        self.assertEqual(baseline['worker_ms'], 5000)
        self.assertEqual(baseline['observation_ms'], 60000)
        for value in ('0', '-1', '1.5', 'invalid', '86401', '999999999999999999999999'):
            bad = subprocess.run([EXE, '--timeout', value], capture_output=True, timeout=10)
            self.assertNotEqual(bad.returncode, 0, value)
            self.assertEqual(bad.stdout, b'', value)
        for arguments in (['--timeout'], ['--timeout','10','--timeout','20'], ['--unexpected']):
            bad = subprocess.run([EXE] + arguments, capture_output=True, timeout=10)
            self.assertNotEqual(bad.returncode, 0)
        maximum = subprocess.run([EXE, '--diagnostics', '--timeout', '86400'],
                                 capture_output=True, text=True, encoding='utf-8', timeout=10)
        self.assertEqual(maximum.returncode, 0, maximum.stderr)
        self.assertEqual(json.loads(maximum.stdout)['timeouts']['worker_ms'], 86400000)


if __name__ == '__main__':
    unittest.main()
