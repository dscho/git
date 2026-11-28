'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { performance } = require('node:perf_hooks');

const [gitArg, rootArg, mode] = process.argv.slice(2);
if (!gitArg || !rootArg || (mode && mode !== '--smoke'))
	throw new Error('usage: node benchmark-parallel-checkout.cjs ' +
			'<git.exe> <new directory> [--smoke]');

const gitPath = path.resolve(gitArg);
const root = path.resolve(rootArg);
const smoke = mode === '--smoke';
const sizes = [50, 99, 100, 150, 199, 200, 250, 299, 300, 350,
	       399, 400, 450, 499, 500, 600];
const sampleSizes = smoke ? [99, 100, 500] : sizes;
const thresholds = [100, 200, 300, 400, 500];
const policies = thresholds.map(threshold => ({
	name: `threshold-${threshold}`, threshold, workers: 4
})).concat([
	{ name: 'direct-serial', threshold: 601, workers: 1 },
	{ name: 'queued-serial', threshold: 601, workers: 4 },
	{ name: 'parallel', threshold: 0, workers: 4 }
]);

fs.mkdirSync(root);
const repo = path.join(root, 'repo');
const output = path.join(root, 'output');
fs.mkdirSync(repo);
fs.mkdirSync(output);
const globalConfig = path.join(root, 'empty-config');
fs.writeFileSync(globalConfig, '');

const env = { ...process.env };
const pathKeys = Object.keys(env).filter(key => /^PATH$/i.test(key));
console.log(`Inherited PATH keys: ${pathKeys.join(', ')}`);
for (const key of Object.keys(env)) {
	if (/^GIT_/i.test(key) || /^PATH$/i.test(key))
		delete env[key];
}
env.GIT_CONFIG_NOSYSTEM = '1';
env.GIT_CONFIG_GLOBAL = globalConfig;
env.GIT_EXEC_PATH = path.dirname(gitPath);
env.PATH = path.dirname(gitPath) + path.delimiter + process.env.PATH;
env.LC_ALL = 'C';

function git(args, extraEnv) {
	const result = spawnSync(gitPath, args, {
		cwd: repo,
		env: extraEnv ? { ...env, ...extraEnv } : env,
		encoding: 'utf8',
		stdio: ['ignore', 'pipe', 'pipe'],
		windowsHide: true
	});
	if (result.error)
		throw result.error;
	if (result.status !== 0)
		throw new Error(`git ${args.join(' ')} failed ` +
				`(${result.status}): ${result.stderr}`);
	return result.stdout.trim();
}

const version = git(['--version']).replace(/^git version /, '');
const execPath = git(['--exec-path']);
if (path.resolve(execPath).toLowerCase() !==
    path.dirname(gitPath).toLowerCase())
	throw new Error('unexpected Git exec path');
console.log(`Git version: ${version}`);
console.log(`Git exec path: ${execPath}`);
git(['init', '--quiet']);
for (const [key, value] of [
	['user.name', 'Checkout Benchmark'],
	['user.email', 'benchmark@example.invalid'],
	['core.autocrlf', 'true'],
	['core.fscache', 'true'],
	['gc.auto', '0']
])
	git(['config', key, value]);

const files = path.join(repo, 'files');
fs.mkdirSync(files);
const body = 'abcdefghijklmnopqrstuvwxyz0123456789\r\n'.repeat(6);
function writeFile(index, changed) {
	const name = `${String(index).padStart(4, '0')}.txt`;
	const text = `${changed ? 'changed' : 'original'} ${index}\r\n${body}`;
	fs.writeFileSync(path.join(files, name), text);
}
const fileCount = smoke ? 600 : 2000;
for (let i = 0; i < fileCount; i++)
	writeFile(i, false);
git(['add', '--', 'files']);
git(['commit', '--quiet', '-m', 'base']);
const base = git(['rev-parse', 'HEAD']);
const targets = new Map();
let previous = 0;
for (const size of sizes) {
	for (let i = previous; i < size; i++)
		writeFile(i, true);
	git(['add', '--', 'files']);
	git(['commit', '--quiet', '-m', `change ${size} files`]);
	const target = git(['rev-parse', 'HEAD']);
	const names = git(['diff', '--name-only', base, target]);
	if (names.split('\n').length !== size)
		throw new Error(`unexpected changed-file count for ${size}`);
	targets.set(size, target);
	previous = size;
}
git(['repack', '-a', '-q']);
console.log(`Repository: ${fileCount} small text files, packed objects`);

function checkout(target, workers, threshold, extraEnv) {
	return git(['-c', `checkout.workers=${workers}`,
		    '-c', `checkout.thresholdForParallelism=${threshold}`,
		    'checkout', '--detach', '--quiet', target], extraEnv);
}
function resetToBase() {
	checkout(base, 1, 601);
}

for (const threshold of thresholds) {
	for (const size of [threshold - 1, threshold]) {
		resetToBase();
		const trace = path.join(output,
					`trace-${threshold}-${size}.jsonl`);
		checkout(targets.get(size), 4, threshold, {
			GIT_TRACE2_EVENT: trace
		});
		const events = fs.readFileSync(trace, 'utf8').trim().split('\n')
			.map(line => JSON.parse(line));
		const spawned = events.filter(event =>
			event.event === 'child_start' &&
			event.argv.includes('checkout--worker'));
		const workers = events.filter(event =>
			event.event === 'start' &&
			event.argv.includes('checkout--worker'));
		const expected = size >= threshold ? 4 : 0;
		if (spawned.length !== expected || workers.length !== expected)
			throw new Error(`worker count mismatch: ` +
					`${size}/${threshold}`);
		const parent = events.find(event => event.event === 'start' &&
					   event.argv.includes('checkout'));
		if (!parent)
			throw new Error('checkout trace missing parent');
		for (const start of [parent, ...workers]) {
			const entry = events.find(event =>
				event.event === 'version' &&
				event.sid === start.sid);
			if (!entry || entry.exe !== version)
				throw new Error(
					'parent or worker version mismatch');
		}
		if (git(['status', '--porcelain']))
			throw new Error('checkout left the worktree dirty');
		console.log(`Verified ${size} files at ${threshold}: ` +
			    `${expected} workers`);
	}
}

let randomState = 0x6469;
function shuffle(items) {
	const result = items.slice();
	for (let i = result.length - 1; i > 0; i--) {
		randomState ^= randomState << 13;
		randomState ^= randomState >>> 17;
		randomState ^= randomState << 5;
		const j = (randomState >>> 0) % (i + 1);
		[result[i], result[j]] = [result[j], result[i]];
	}
	return result;
}

const csv = path.join(output, 'results.csv');
fs.writeFileSync(csv,
	'round,block,position,files,policy,threshold,workers,elapsed_ms\n');
const started = performance.now();
let round = 0;
do {
	round++;
	for (const [block, size] of shuffle(sampleSizes).entries()) {
		const rows = [];
		for (const [position, policy] of shuffle(policies).entries()) {
			resetToBase();
			const start = performance.now();
			checkout(targets.get(size),
				 policy.workers, policy.threshold);
			const elapsed = performance.now() - start;
			rows.push([
				round, block + 1, position + 1, size,
				policy.name, policy.threshold, policy.workers,
				elapsed.toFixed(3)
			].join(','));
		}
		fs.appendFileSync(csv, rows.join('\n') + '\n');
	}
	console.log(`Completed round ${round}`);
} while (!smoke && round < 20 &&
	 (round < 4 || performance.now() - started < 540000));
if (git(['status', '--porcelain']))
	throw new Error('checkout left the worktree dirty');
console.log(`Raw timings: ${csv}`);
