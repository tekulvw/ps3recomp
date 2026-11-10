"""Exercise the installer without allowing package-manager changes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/install-linux-dependencies.sh'


class InstallLinuxDependenciesTests(unittest.TestCase):
    def run_installer(self, installed='', config_only='', fail_update=False, packages=('ninja-build', 'clang')):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            commands = {
                'dpkg-query': '''#!/bin/bash
for package; do :; done
case " $INSTALLED " in *" $package "*) printf 'install ok installed'; exit 0;; esac
case " $CONFIG_ONLY " in *" $package "*) printf 'deinstall ok config-files'; exit 0;; esac
exit 1
''',
                'sudo': '''#!/bin/bash
printf 'APT:%s\\n' "$*"
if [[ "$*" == 'apt-get update' && "$FAIL_UPDATE" == 1 ]]; then exit 42; fi
''',
            }
            for name, content in commands.items():
                path = directory / name
                path.write_text(content)
                path.chmod(0o755)
            return subprocess.run(
                ['/bin/bash', str(SCRIPT), *packages], text=True, capture_output=True,
                env={**os.environ, 'PATH': f'{directory}:/usr/bin:/bin', 'INSTALLED': installed,
                     'CONFIG_ONLY': config_only, 'FAIL_UPDATE': '1' if fail_update else '0'},
            )

    def test_preinstalled_packages_skip_all_apt_operations(self):
        result = self.run_installer(installed='ninja-build clang')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('APT:', result.stdout)
        self.assertIn('skipping apt', result.stdout)

    def test_installs_only_missing_packages(self):
        result = self.run_installer(installed='ninja-build')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([line for line in result.stdout.splitlines() if line.startswith('APT:')],
                         ['APT:apt-get update', 'APT:apt-get install -y clang'])

    def test_removed_package_with_config_files_is_not_installed(self):
        result = self.run_installer(installed='ninja-build', config_only='clang')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('APT:apt-get install -y clang', result.stdout)

    def test_update_failure_stops_before_install(self):
        result = self.run_installer(fail_update=True)
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertNotIn('APT:apt-get install', result.stdout)

    def test_no_packages_is_an_error_without_apt(self):
        result = self.run_installer(packages=())
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNotIn('APT:', result.stdout)


if __name__ == '__main__':
    unittest.main()
