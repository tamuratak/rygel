#!/usr/bin/env node

// Copyright 2023 Niels Martignène <niels.martignene@protonmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the “Software”), to deal in 
// the Software without restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
// Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

'use strict';

const process = require('process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const util = require('util');
const { spawn, spawnSync, execFileSync } = require('child_process');
const { NodeSSH } = require('node-ssh');
const chalk = require('chalk');
const minimatch = require('minimatch');
const esbuild = require('esbuild');
const tar = require('tar');

// Globals

let script_dir = null;
let root_dir = null;

let machines = null;
let keyboard_layout = null;
let accelerate = true;
let ignore = new Set;

let qemu_prefix = null;

// Main

main();

async function main() {
    script_dir = fs.realpathSync(__dirname);
    root_dir = fs.realpathSync(script_dir + '/../../..');

    // All the code assumes we are working from the script directory
    process.chdir(script_dir);

    let command = test;
    let patterns = [];

    // Parse options
    {
        if (process.argv[2] == '--help') {
            print_usage();
            return;
        }
        if (process.argv.length < 3 || process.argv[2][0] == '-')
            throw new Error(`Missing command, use --help`);

        switch (process.argv[2]) {
            case 'test': { command = test; } break;
            case 'debug': { command = debug; } break;
            case 'build':
            case 'dist': { command = build; } break;
            case 'start': { command = start; } break;
            case 'stop': { command = stop; } break;
            case 'info': { command = info; } break;
            case 'ssh': { command = ssh; } break;
            case 'reset': { command = reset; } break;

            default: { throw new Error(`Unknown command '${process.argv[2]}'`); } break;
        }

        for (let i = 3; i < process.argv.length; i++) {
            let arg = process.argv[i];
            let value = null;

            if (arg[0] == '-') {
                if (arg.length > 2 && arg[1] != '-') {
                    value = arg.substr(2);
                    arg = arg.substr(0, 2);
                } else if (arg[1] == '-') {
                    let offset = arg.indexOf('=');

                    if (offset > 2 && arg.length > offset + 1) {
                        value = arg.substr(offset + 1);
                        arg = arg.substr(0, offset);
                    }
                }
                if (value == null && process.argv[i + 1] != null && process.argv[i + 1][0] != '-') {
                    value = process.argv[i + 1];
                    i++; // Skip this value next iteration
                }
            }

            if (arg == '--help') {
                print_usage();
                return;
            } else if ((command == test || command == start) && (arg == '-k' || arg == '--keyboard')) {
                if (value == null)
                    throw new Error(`Missing value for ${arg}`);

                keyboard_layout = value;
            } else if ((command == test || command == start) && arg == '--no_accel') {
                accelerate = false;
            } else if (arg[0] == '-') {
                throw new Error(`Unexpected argument '${arg}'`);
            } else {
                if (arg.startsWith('__') || arg.match(/[\\/\.]/))
                    throw new Error(`Machine name '${arg} is not valid`);

                patterns.push(arg);
            }
        }
    }

    // Sanity checks
    if (parseInt(process.versions.node, 10) < 16)
        throw new Error('Please use Node version >= 16');
    if (spawnSync('ssh', ['-V']).status !== 0)
        throw new Error('Missing ssh binary in PATH');

    // Load machine registry
    let machines_map;
    {
        let json = fs.readFileSync('registry/machines.json', { encoding: 'utf-8' });

        machines_map = JSON.parse(json);
        for (let key in machines_map) {
            let machine = machines_map[key];

            machine.key = key;
            machine.started = false;

            if (machine.qemu != null) {
                machine.qemu.accelerate = null;

                if (accelerate && (machine.qemu.binary == 'qemu-system-x86_64' ||
                                   machine.qemu.binary == 'qemu-system-i386')) {
                    if (process.platform == 'linux') {
                        machine.qemu.accelerate = 'kvm';
                    } else if (process.platform == 'win32') {
                        machine.qemu.accelerate = 'whpx';
                    }
                }
            }
        }
    }

    if (patterns.length) {
        machines = new Set;

        for (let pattern of patterns) {
            let re = minimatch.makeRe(pattern);
            let match = false;

            for (let name in machines_map) {
                let machine = machines_map[name];

                if (name.match(re) || machine.name.match(re)) {
                    machines.add(name);
                    match = true;
                }
            }

            if (!match) {
                console.log(`Pattern '${pattern}' does not match any machine`);
                process.exit(1);
            }
        }

    } else {
        machines = new Set(Object.keys(machines_map));

        if (!machines.size) {
            console.error('Could not detect any machine');
            process.exit(1);
        }
    }

    machines = Array.from(machines);
    machines = machines.map(name => {
        let machine = machines_map[name];
        if (machine == null) {
            machine = Object.values(machines_map).find(machine => machine.name == name);
            if (machine == null)
                throw new Error(`Could not find machine ${name}`);
        }
        return machine;
    });

    console.log('Machines:', machines.map(machine => machine.name).join(', '));
    console.log();

    try {
        let success = !!(await command());
        process.exit(!success);
    } catch (err) {
        console.error(err);
        process.exit(1);
    }
}

function print_usage() {
    let help = `Usage: node qemu.js <command> [options...]

Commands:
    test                         Run the machines and perform the tests (default)
    debug                        Run the machines and perform the tests, with debug build

    dist                         Prepare distribution-ready NPM package directory

    start                        Start the machines but don't run anythingh
    stop                         Stop running machines
    info                         Print basic information about machine
    ssh                          Start SSH session with specific machine

    reset                        Reset initial disk snapshot

Options:
    -k, --keyboard <LAYOUT>      Set VNC keyboard layout

        --no_accel               Disable QEMU acceleration
`;

    console.log(help);
}

// Commands

async function start(detach = true) {
    let success = true;
    let missing = 0;

    check_qemu();

    console.log('>> Starting up machines...');
    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;
        if (machine.qemu == null) {
            ignore.add(machine);
            missing++;

            return;
        }

        let dirname = `qemu/${machine.key}`;

        if (!fs.existsSync(dirname)) {
            log(machine, 'Missing files', chalk.bold.gray('[ignore]'));

            ignore.add(machine);
            missing++;

            return;
        }

        // Version check
        {
            let filename = dirname + '/VERSION';
            let version = fs.existsSync(filename) ? parseInt(fs.readFileSync(filename).toString(), 10) : 0;

            if (version < machine.qemu.version) {
                log(machine, 'Machine version mismatch', chalk.bold.gray('[ignore]'));

                ignore.add(machine);
                success = false;

                return;
            }
        }

        try {
            await boot(machine, dirname, detach);

            if (machine.started) {
                let action = `Start (${machine.qemu.accelerate || 'emulated'})`;
                log(machine, action, chalk.bold.green('[ok]'));
            } else {
                log(machine, 'Join', chalk.bold.green('[ok]'));
            }
        } catch (err) {
            log(machine, 'Start', chalk.bold.red('[error]'));

            ignore.add(machine);
            success = false;
        }
    }));

    if (success && missing == machines.length)
        throw new Error('No machine available');

    return success;
}

async function build() {
    let dist_dir = script_dir + '/../build/dist';
    let snapshot_dir = snapshot();

    let json = fs.readFileSync(root_dir + '/src/koffi/package.json', { encoding: 'utf-8' });
    let version = JSON.parse(json).version;

    console.log('>> Version:', version);
    console.log('>> Checking build archives...');
    for (let machine of machines) {
        let needed = false;

        for (let suite in machine.builds) {
            let build = machine.builds[suite];

            let binary_filename = root_dir + `/src/koffi/build/qemu/${version}/${machine.platform}_${build.arch}/koffi.node`;

            if (fs.existsSync(binary_filename)) {
                log(machine, `${suite} > Status`, chalk.bold.green(`[ok]`));
            } else if (machine.qemu == null) {
                log(machine, `${suite} > Status`, chalk.bold.red(`[manual]`));
                needed = true;
            } else {
                log(machine, `${suite} > Status`, chalk.bold.red(`[missing]`));
                needed = true;
            }
        }

        if (!needed)
            ignore.add(machine);
    }

    let ready = ignore.size;
    let artifacts = [];

    // Run machine commands
    {
        let success = true;

        if (ready < machines.length) {
            success &= await start(false);
            success &= await upload(snapshot_dir, machine => Object.values(machine.builds).map(build => build.directory));

            console.log('>> Run build commands...');
            await compile();
        }

        console.log('>> Get build artifacts');
        {
            let build_dir = root_dir + '/src/koffi/build/qemu';

            await Promise.all(machines.map(async machine => {
                let copied = true;

                await Promise.all(Object.keys(machine.builds).map(async suite => {
                    let build = machine.builds[suite];

                    let src_dir = build.directory + `/src/koffi/build/koffi/${machine.platform}_${build.arch}`;
                    let dest_dir = build_dir + `/${version}/${machine.platform}_${build.arch}`;

                    artifacts.push(dest_dir);

                    if (ignore.has(machine))
                        return;

                    unlink_recursive(dest_dir);
                    fs.mkdirSync(dest_dir, { mode: 0o755, recursive: true });

                    try {
                        await machine.ssh.getDirectory(dest_dir, src_dir, {
                            recursive: false,
                            concurrency: 4,
                            validate: filename => !path.basename(filename).match(/^v[0-9]+/)
                        });
                    } catch (err) {
                        ignore.add(machine);
                        success = false;
                        copied = false;
                    }
                }));

                let status = copied ? chalk.bold.green('[ok]') : chalk.bold.red('[error]');
                log(machine, 'Pack', status);
            }));
        }

        if (machines.some(machine => machine.started))
            success &= await stop(false);
        success &= (ignore.size == ready);

        if (!success) {
            console.log('');
            console.log('>> Status: ' + chalk.bold.red('FAILED'));
            return null;
        }
    }

    console.log('>> Prepare NPM package');
    {
        let build_dir = dist_dir + '/build/koffi';

        unlink_recursive(dist_dir);
        fs.mkdirSync(dist_dir, { mode: 0o755, recursive: true });

        copy_recursive(snapshot_dir, dist_dir, filename => {
            let parts = filename.split('/');

            if (parts[0] == 'src' && parts[1] == 'koffi') {
                return parts[2] != 'benchmark' &&
                       parts[2] != 'test' &&
                       parts[2] != 'tools';
            } else if (parts[0] == 'src') {
                return true;
            } else if (parts[0] == 'vendor') {
                return parts[1] == null ||
                       parts[1] == 'node-addon-api';
            } else if (parts[0] == 'web') {
                return true;
            } else {
                return false;
            }
        });
        fs.mkdirSync(build_dir, { mode: 0o755, recursive: true });

        for (let artifact of artifacts) {
            let dest_dir = build_dir + '/' + path.basename(artifact);

            fs.mkdirSync(dest_dir, { mode: 0o755 });
            copy_recursive(artifact, dest_dir);
        }

        let pkg = JSON.parse(json);

        pkg.scripts = {
            install: 'node src/cnoke/cnoke.js -p . -d src/koffi --prebuild'
        };
        delete pkg.devDependencies;

        esbuild.buildSync({
            entryPoints: [dist_dir + '/src/koffi/index.js'],
            bundle: true,
            minify: false,
            write: true,
            platform: 'node',
            outfile: dist_dir + '/index.js'
        });
        esbuild.buildSync({
            entryPoints: [dist_dir + '/src/koffi/indirect.js'],
            bundle: true,
            minify: false,
            write: true,
            platform: 'node',
            outfile: dist_dir + '/indirect.js'
        });

        fs.writeFileSync(dist_dir + '/package.json', JSON.stringify(pkg, null, 4));
        fs.unlinkSync(dist_dir + '/src/koffi/package.json');
        fs.unlinkSync(dist_dir + '/src/koffi/index.js');
        fs.unlinkSync(dist_dir + '/src/koffi/indirect.js');
        fs.renameSync(dist_dir + '/src/koffi/index.d.ts', dist_dir + '/index.d.ts');
        fs.unlinkSync(dist_dir + '/src/koffi/.gitignore');
        fs.renameSync(dist_dir + '/src/koffi/README.md', dist_dir + '/README.md');
        fs.renameSync(dist_dir + '/src/koffi/LICENSE.txt', dist_dir + '/LICENSE.txt');
        fs.renameSync(dist_dir + '/src/koffi/CHANGELOG.md', dist_dir + '/CHANGELOG.md');
        fs.renameSync(dist_dir + '/web/koffi.dev', dist_dir + '/doc');
        fs.rmdirSync(dist_dir + '/web');
    }

    console.log('>> Test prebuild');
    {
        let pkg = JSON.parse(fs.readFileSync(dist_dir + '/package.json'));
        let require_filename = path.join(dist_dir, expand_path(pkg.cnoke.require, pkg.version));

        let proc = spawnSync(process.execPath, ['-e', 'require(process.argv[1])', require_filename]);
        if (proc.status !== 0)
            throw new Error('Failed to use prebuild:\n' + (proc.stderr || proc.stdout));
    }

    return dist_dir;
}

async function compile(debug = false) {
    let success = true;

    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;

        await Promise.all(Object.keys(machine.builds).map(async suite => {
            let build = machine.builds[suite];

            let cmd = build.build + (debug ? ' --debug' : ' --config release');
            let cwd = build.directory + '/src/koffi';

            let start = process.hrtime.bigint();
            let ret = await exec_remote(machine, cmd, cwd);
            let time = Number((process.hrtime.bigint() - start) / 1000000n);

            if (ret.code == 0) {
                log(machine, `${suite} > Build`, chalk.bold.green(`[${(time / 1000).toFixed(2)}s]`));
            } else {
                log(machine, `${suite} > Build`, chalk.bold.red('[error]'));

                if (ret.stdout || ret.stderr)
                    console.error('');

                let align = log.align + 9;
                if (ret.stdout) {
                    let str = ' '.repeat(align) + 'Standard output:\n' +
                              chalk.yellow(ret.stdout.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                    console.error(str);
                }
                if (ret.stderr) {
                    let str = ' '.repeat(align) + 'Standard error:\n' +
                              chalk.yellow(ret.stderr.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                    console.error(str);
                }

                ignore.add(machine);
                success = false;
            }
        }));
    }));

    return success;
}

function snapshot() {
    let snapshot_dir = script_dir + '/../build/snapshot';

    unlink_recursive(snapshot_dir);
    fs.mkdirSync(snapshot_dir, { mode: 0o755, recursive: true });

    console.log('>> Snapshot code...');
    copy_recursive(root_dir, snapshot_dir, filename => {
        let parts = filename.split('/');

        if (parts[0] == 'src' && parts[1] == 'core') {
            return parts[2] == null || parts[2] == 'base';
        } else if (parts[0] == 'src') {
            return parts[1] == null || parts[1] == 'cnoke' ||
                                       parts[1] == 'koffi';
        } else if (parts[0] == 'tools') {
            return parts[1] == null || parts[1] != 'qemu';
        } else if (parts[0] == 'vendor') {
            return parts[1] == null || parts[1] == 'node-addon-api' ||
                                       parts[1] == 'raylib' ||
                                       parts[1] == 'sqlite3' ||
                                       parts[1] == 'sqlite3mc';
        } else if (parts[0] == 'web') {
            return parts[1] == null || parts[1] == 'koffi.dev';
        } else {
            return false;
        }
    });

    return snapshot_dir;
}

async function upload(snapshot_dir, func) {
    let success = true;

    console.log('>> Upload source code...');
    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;

        let copied = true;

        for (let directory of func(machine)) {
            for (let i = 0; i < 10; i++) {
                try {
                    await machine.ssh.exec('rm', ['-rf', directory]);
                    break;
                } catch (err) {
                    // Fails often on Windows (busy directory or whatever), but rarely a problem

                    await wait(1000);
                    continue;
                }
            }

            try {
                await machine.ssh.putDirectory(snapshot_dir, directory, {
                    recursive: true,
                    concurrency: (process.platform != 'win32') ? 4 : 1
                });
            } catch (err) {
                ignore.add(machine);
                success = false;
                copied = false;
            }
        }

        let status = copied ? chalk.bold.green('[ok]') : chalk.bold.red('[error]');
        log(machine, 'Upload', status);
    }));

    return success;
}

async function debug() {
    let success = await test(true);
    return success;
}

async function test(debug = false) {
    let snapshot_dir = snapshot();

    let success = true;

    success &= await start(false);
    success &= await upload(snapshot_dir, machine => Object.values(machine.builds).map(build => build.directory));

    // Errors beyond here are actual failures
    let ignored = ignore.size;

    console.log('>> Run build commands...');
    await compile(debug);

    console.log('>> Run test commands...');
    await Promise.all(machines.map(async machine => {
        if (ignore.has(machine))
            return;

        await Promise.all(Object.keys(machine.tests).map(async suite => {
            let test = machine.tests[suite];
            let commands = {
                'Build': test.build + (debug ? ' --debug' : ' --config release'),
                ...test.commands
            };

            for (let name in commands) {
                let cmd = commands[name];
                let cwd = test.directory + '/src/koffi';

                let start = process.hrtime.bigint();
                let ret = await exec_remote(machine, cmd, cwd);
                let time = Number((process.hrtime.bigint() - start) / 1000000n);

                if (ret.code === 0) {
                    log(machine, `${suite} > ${name}`, chalk.bold.green(`[${(time / 1000).toFixed(2)}s]`));
                } else {
                    log(machine, `${suite} > ${name}`, chalk.bold.red('[error]'));

                    if (ret.stdout || ret.stderr)
                        console.error('');

                    let align = log.align + 9;
                    if (ret.stdout) {
                        let str = ' '.repeat(align) + 'Standard output:\n' +
                                  chalk.yellow(ret.stdout.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                        console.error(str);
                    }
                    if (ret.stderr) {
                        let str = ' '.repeat(align) + 'Standard error:\n' +
                                  chalk.yellow(ret.stderr.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                        console.error(str);
                    }

                    success = false;

                    if (name == 'Build')
                        break;
                }
            }
        }));
    }));

    if (machines.some(machine => machine.started))
        success &= await stop(false);

    // Build failures need to register as errors
    success &= (ignore.size == ignored);

    if (success) {
        console.log('>> Status: ' + chalk.bold.green('SUCCESS'));
        if (ignore.size)
            console.log('   (but some machines could not be tested)');
    } else {
        console.log('>> Status: ' + chalk.bold.red('FAILED'));
    }

    return success;
}

async function stop(all = true) {
    let success = true;

    console.log('>> Sending shutdown commands...');
    await Promise.all(machines.map(async machine => {
        if (machine.qemu == null)
            return;
        if (!machine.started && !all)
            return;

        if (machine.ssh == null) {
            try {
                await join(machine, 2);
            } catch (err) {
                log(machine, 'Already down', chalk.bold.green('[ok]'));
                return;
            }
        }

        try {
            await new Promise(async (resolve, reject) => {
                if (machine.ssh.connection == null) {
                    reject();
                    return;
                }

                machine.ssh.connection.on('close', resolve);
                machine.ssh.connection.on('end', resolve);
                wait(60000).then(() => { reject(new Error('Timeout')) });

                exec_remote(machine, machine.qemu.shutdown);
            });

            log(machine, 'Stop', chalk.bold.green('[ok]'));
        } catch (err) {
            log(machine, 'Stop', chalk.bold.red('[error]'));
            success = false;
        }
    }));

    return success;
}

async function info() {
    for (let machine of machines) {
        if (machine.qemu == null)
            continue;

        console.log(`>> ${machine.name} (${machine.key})`);
        console.log(`  - SSH port: ${machine.qemu.ssh_port}`);
        console.log(`  - VNC port: ${machine.qemu.vnc_port}`);
        console.log(`  - Username: ${machine.qemu.username}`);
        console.log(`  - Password: ${machine.qemu.password}`);
    }
}

async function ssh() {
    if (machines.length != 1) {
        console.error('The ssh command can only be used with one machine');
        return false;
    }

    let machine = machines[0];

    let args = [
        '-p' + machine.qemu.password,
        'ssh', '-o', 'StrictHostKeyChecking=no',
               '-o', 'UserKnownHostsFile=' + (process.platform == 'win32' ? '\\\\.\\NUL' : '/dev/null'),
               '-p', machine.qemu.ssh_port, machine.qemu.username + '@127.0.0.1'
    ];

    let proc = spawnSync('sshpass', args, { stdio: 'inherit' });
    if (proc.status !== 0) {
        console.error('Connection failed');
        return false;
    }

    return true;
}

async function reset() {
    check_qemu();

    let binary = qemu_prefix + 'qemu-img' + (process.platform == 'win32' ? '.exe' : '');

    console.log('>> Restoring snapshots...')
    await Promise.all(machines.map(machine => {
        if (machine.qemu == null)
            return;

        let dirname = `qemu/${machine.key}`;
        let disk = dirname + '/' + machine.qemu.disk;

        if (!fs.existsSync(dirname)) {
            log(machine, 'Missing files', chalk.bold.gray('[ignore]'));
            return;
        }

        let proc = spawnSync(binary, ['snapshot', disk, '-a', 'base'], { encoding: 'utf-8' });

        if (proc.status === 0) {
            log(machine, 'Reset disk', chalk.bold.green('[ok]'));
        } else {
            log(machine, 'Reset disk', chalk.bold.red('[error]'));

            if (proc.stderr) {
                console.error('');

                let align = log.align + 9;
                let str = ' '.repeat(align) + 'Standard error:\n' +
                          chalk.yellow(proc.stderr.replace(/^/gm, ' '.repeat(align + 4))) + '\n';
                console.error(str);
            }
        }
    }));
}

// Utility

function check_qemu() {
    if (qemu_prefix != null)
        return;

    if (spawnSync('qemu-img', ['--version']).status === 0) {
        qemu_prefix = '';
    } else if (process.platform == 'win32') {
        let proc = spawnSync('reg', ['query', 'HKEY_LOCAL_MACHINE\\SOFTWARE\\QEMU', '/v', 'Install_Dir']);

        if (proc.status === 0) {
            let matches = proc.stdout.toString('utf-8').match(/Install_Dir[ \t]+REG_[A-Z_]+[ \t]+(.*)+/);

            if (matches != null) {
                let prefix = matches[1].trim() + '/';
                let binary = prefix + 'qemu-img.exe';

                if (fs.existsSync(binary))
                    qemu_prefix = prefix;
            }
        }
    }

    if (qemu_prefix == null)
        throw new Error('QEMU does not seem to be installed');
}

function copy_recursive(src, dest, validate = filename => true) {
    let proc = spawnSync('git', ['ls-files', '-i', '-o', '--exclude-standard', '--directory'], { cwd: src });
    let ignored = new Set(proc.stdout.toString().split('\n').map(it => it.trim().replace(/[\\\/+]$/, '').replaceAll('\\', '/')).filter(it => it));

    recurse(src, dest, '');

    function recurse(src, dest, nice) {
        let entries = fs.readdirSync(src, { withFileTypes: true });

        for (let entry of entries) {
            let src_filename = path.join(src, entry.name);
            let dest_filename = path.join(dest, entry.name);
            let filename = nice + (nice ? '/' : '') + entry.name;

            if (ignored.has(filename))
                continue;
            if (!validate(filename))
                continue;

            if (entry.isDirectory()) {
                fs.mkdirSync(dest_filename, { mode: 0o755 });
                recurse(src_filename, dest_filename, filename);
            } else if (entry.isFile()) {
                fs.copyFileSync(src_filename, dest_filename);
            }
        }
    }
}

function unlink_recursive(path) {
    try {
        if (fs.rmSync != null) {
            fs.rmSync(path, { recursive: true, maxRetries: process.platform == 'win32' ? 3 : 0 });
        } else {
            fs.rmdirSync(path, { recursive: true, maxRetries: process.platform == 'win32' ? 3 : 0 });
        }
    } catch (err) {
        if (err.code !== 'ENOENT')
            throw err;
    }
}

async function boot(machine, dirname, detach) {
    let args = machine.qemu.arguments.slice();

    if (keyboard_layout != null)
        args.push('-k', keyboard_layout);
    if (machine.qemu.accelerate)
        args.push('-accel', machine.qemu.accelerate);
    // args.push('-display', 'none');

    try {
        let binary = qemu_prefix + machine.qemu.binary + (process.platform == 'win32' ? '.exe' : '');

        let proc = spawn(binary, args, {
            cwd: dirname,
            detached: detach,
            stdio: 'ignore'
        });
        if (detach)
            proc.unref();

        await new Promise((resolve, reject) => {
            proc.on('spawn', () => wait(2 * 1000).then(resolve));
            proc.on('error', reject);
            proc.on('exit', reject);
        });

        await join(machine, 30);
        machine.started = true;
    } catch (err) {
        if (typeof err != 'number')
            throw err;

        await join(machine, 2);
        machine.started = false;
    }
}

async function join(machine, tries) {
    let ssh = new NodeSSH;

    while (tries) {
        try {
            await ssh.connect({
                host: '127.0.0.1',
                port: machine.qemu.ssh_port,
                username: machine.qemu.username,
                password: machine.qemu.password,
                tryKeyboard: true
            });

            break;
        } catch (err) {
            if (!--tries)
                throw new Error(`Failed to connect to ${machine.name}`);

            // Try again... a few times
            await wait(10 * 1000);
        }
    }

    machine.ssh = ssh;
}

function wait(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function log(machine, action, status) {
    if (log.align == null) {
        let lengths = machines.map(machine => machine.name.length);
        log.align = Math.max(...lengths);
    }

    let align1 = Math.max(log.align - machine.name.length, 0);
    let align2 = Math.max(34 - action.length, 0);

    console.log(`     [${machine.name}]${' '.repeat(align1)}  ${action}${' '.repeat(align2)}  ${status}`);
}

async function exec_remote(machine, cmd, cwd = null) {
    if (typeof cmd == 'string') {
        cmd = {
            command: cmd,
            repeat: 1
        };
    }

    try {
        let ret = { code: 0 };

        if (machine.platform == 'win32') {
            let cmd_line = cmd.command;

            if (cwd != null) {
                cwd = cwd.replaceAll('/', '\\');
                cmd_line = `cd "${cwd}" && ${cmd_line}`;
            }

            for (let i = 0; ret.code === 0 && i < cmd.repeat; i++)
                ret = await machine.ssh.execCommand(cmd_line);
        } else {
            for (let i = 0; ret.code === 0 && i < cmd.repeat; i++)
                ret = await machine.ssh.execCommand(cmd.command, { cwd: cwd });
        }

        return ret;
    } catch (err) {
        console.log(err);
        return err;
    }
}

function expand_path(str, version) {
    let ret = str.replace(/{{ *([a-zA-Z_][a-zA-Z_0-9]*) *}}/g, (match, p1) => {
        switch (p1) {
            case 'version': return version;
            case 'platform': return process.platform;
            case 'arch': return arch;

            default: return match;
        }
    });

    return ret;
}
