"""AI-authored explicit real-VM installer acceptance; never included in releases."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('host', 'fixtures', 'vmx', 'output'):
        parser.add_argument('--' + argument, type=Path, required=True)
    parser.add_argument('--snapshot', required=True)
    parser.add_argument('--confirm', required=True)
    options = parser.parse_args()
    if options.confirm != 'DISPOSABLE_VM_INSTALLER_TEST':
        parser.error('Explicit disposable-VM confirmation required')
    host, fixtures, vmx, out = [getattr(options, k).resolve() for k in ('host', 'fixtures', 'vmx', 'output')]
    if not vmx.is_file() or not host.is_file():
        raise ValueError('Expected existing Host and explicitly selected VMX')
    out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    config = out / 'lab.local.json'
    counter = 0
    def command(*args, allowed=(0,), timeout=240):
        nonlocal counter
        counter += 1
        result = subprocess.run([str(host), *map(str, args)], capture_output=True, timeout=timeout,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        (out / f'{counter:03}-stdout.json').write_bytes(result.stdout)
        (out / f'{counter:03}-stderr.log').write_bytes(result.stderr)
        with (out / 'commands.jsonl').open('a', encoding='utf-8') as log:
            log.write(json.dumps({'index': counter, 'arguments': list(map(str, args)), 'exit': result.returncode}) + '\n')
        if result.returncode not in allowed:
            raise RuntimeError(f'Host command {counter} failed ({result.returncode}): {result.stderr.decode("utf-8", errors="replace")}')
        return json.loads(result.stdout.decode('utf-8-sig'))

    command('--version', '--json')
    command('init', '--config', config, '--vmx', vmx, '--base-snapshot', options.snapshot,
            '--agent-binary', fixtures / 'SatsumaVM.exe')
    lab = json.loads(config.read_text(encoding='utf-8-sig'))
    vm = lab['vms'][0]
    if command('lab', 'status', '--config', config)['status'] != 'available':
        raise RuntimeError('Lab unavailable')
    before = command('snapshot', 'create-ai', '--config', config, '--vm', vm['id'], '--name', 'installer-before')
    (out / 'before-snapshot.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    suffix = uuid.uuid4().hex[:12]
    context = {'confirm': options.confirm, 'hardware_id': vm['hardware_id'],
               'agent_sha256': hashlib.sha256((fixtures / 'SatsumaVM.exe').read_bytes()).hexdigest(),
               'test_root': 'C:\\ProgramData\\SatsumaAcceptance-' + suffix,
               'task_name': 'SatsumaAcceptance-' + suffix}
    (out / 'context.json').write_text(json.dumps(context, indent=2), encoding='utf-8')
    def plan(name, script, files, collected=(), run_as='system'):
        run = 'installer-' + name + '-' + suffix
        artifacts = [{'source': str(path), 'vm': vm['id'], 'destination': 'artifacts/' + path.name} for path in files]
        value = {'schema_version': 3, 'name': name, 'run_id': run, 'artifacts': artifacts,
                 'steps': [{'id': name, 'vm': vm['id'], 'type': 'script', 'engine': 'windows_powershell',
                            'script': 'artifacts/' + script, 'run_as': run_as, 'timeout_seconds': 90,
                            'retry_safe': False, 'collect_files': list(collected)}],
                 'lifecycle': {'vms': [{'vm': vm['id'], 'on_success': {'action': 'stop'}, 'on_failure': {'action': 'stop'}}]},
                 'cleanup': {'guest_work': {'on_success': 'delete', 'on_failure': 'retain'},
                             'host_run': {'on_success': 'archive_then_delete', 'on_failure': 'retain'}}}
        path = out / (name + '-plan.json')
        path.write_text(json.dumps(value, indent=2), encoding='utf-8')
        command('plan', 'validate', '--config', config, '--plan', path)
        return path
    gateway = None
    logs = []
    def start_gateway():
        nonlocal gateway
        log = (out / ('gateway-' + str(len(logs)) + '.log')).open('wb')
        logs.append(log)
        gateway = subprocess.Popen([str(host), 'gateway', '--config', str(config)], stdout=log, stderr=log,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
    def stop_gateway():
        nonlocal gateway
        if gateway is not None:
            gateway.terminate()
            gateway.wait(timeout=15)
            gateway = None
    summary = {'passed': False, 'vm_id': vm['id'], 'before_snapshot': before['snapshot']}
    try:
        command('vm', 'restore', '--config', config, '--id', vm['id'], '--snapshot', options.snapshot)
        start_gateway()
        command('vm', 'start', '--config', config, '--id', vm['id'])
        ready = command('check', '--config', config, '--timeout-seconds', 180)
        if ready['status'] != 'ready':
            raise RuntimeError('Initial check not ready')
        inventory = command('agent', 'inventory', '--config', config, '--vm', vm['id'])
        (out / 'inventory.json').write_text(json.dumps(inventory, indent=2), encoding='utf-8')
        bootstrap = plan('bootstrap', 'real_vmware_installer_bootstrap.ps1',
                         [*fixtures.iterdir(), out / 'context.json', source / 'real_vmware_installer_guest.ps1',
                          source / 'real_vmware_installer_bootstrap.ps1'])
        result = command('orchestrate', '--config', config, '--plan', bootstrap, '--timeout-seconds', 300, timeout=330)
        if result['status'] != 'COMPLETED':
            raise RuntimeError('Bootstrap failed')
        stop_gateway()
        command('vm', 'start', '--config', config, '--id', vm['id'])
        print('Installer matrix running offline in Guest; Host gateway is stopped.', flush=True)
        for elapsed in range(0, 180, 30):
            time.sleep(30)
            print(f'Offline installer observation: {elapsed + 30}/180 seconds.', flush=True)
        start_gateway()
        if command('check', '--config', config, '--timeout-seconds', 180)['status'] != 'ready':
            raise RuntimeError('Agent did not return after installer matrix')
        for name, script, collected, identity in [
            ('collect', 'real_vmware_installer_collect.ps1', ['installer-results.json'], 'system'),
            ('permissions', 'real_vmware_permissions_guest.ps1', ['permissions.json'], 'interactive_user')]:
            task = plan(name, script, [source / script, out / 'context.json'], collected, identity)
            result = command('orchestrate', '--config', config, '--plan', task, '--timeout-seconds', 300,
                             '--report-out', out / (name + '-orchestrate.json'), allowed=(0, 1), timeout=330)
            report = command('report', '--config', config, '--run', result['run_id'], allowed=(0, 1))
            (out / (name + '-report.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
            if result['status'] != 'COMPLETED':
                raise RuntimeError(f'{name} failed; retained exact evidence in run {result["run_id"]}')
            if report['source'] != 'archive' or not report['complete']:
                raise RuntimeError('Expected complete independent archive')
            base = Path(lab['host']['archive_root']) / 'runs' / result['run_id'] / 'evidence/main'
            for execution in report['executions']:
                for f in execution['files']:
                    content = (base / f['path']).read_bytes()
                    assert hashlib.sha256(content).hexdigest() == f['sha256']
                    (out / Path(f['path']).name).write_bytes(content)
            summary[name] = 'passed'
            print(name + ': passed with archived evidence.', flush=True)
        summary['passed'] = True
    except Exception as error:
        summary['error'] = str(error)
        raise
    finally:
        stop_gateway()
        for log in logs:
            log.close()
        status = command('lab', 'status', '--config', config)
        if status['status'] == 'available':
            command('vm', 'stop', '--config', config, '--id', vm['id'])
            summary['restoration'] = command('vm', 'restore', '--config', config, '--id', vm['id'], '--snapshot', before['snapshot'])
        else:
            summary['restoration'] = {'status': 'blocked', 'lab': status}
        (out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
