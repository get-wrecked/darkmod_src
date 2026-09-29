#!/usr/bin/env python3
"""End-to-end regression of the arcade integration through the debug app's HTTP API.

Run the game with the SDK enabled (see README.md, "Local testing") and the debug
app on port 6060 (`arcade-sdk debug --sdk-addr 127.0.0.1:6006 --port 6060 --no-browser`),
then:  python3 e2e_test.py [--api http://127.0.0.1:6060/api/]

It starts every challenge type, drives the outcome with the game's own RPCs
(teleports, console commands), and checks the ChallengeCompleted reports. Takes
about a minute; the game ends up idle at the main menu of the last map loaded.
"""
import argparse, json, sys, time, urllib.request

API = 'http://127.0.0.1:6060/api/'
failures = []


def post(ep, body, timeout=120):
    req = urllib.request.Request(API + ep, data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
    return json.load(urllib.request.urlopen(req, timeout=timeout))


def call(method, request=None):
    r = post("call", {"instance": 0, "service": "thedarkmod.v1.Game", "method": method, "request": request or {}, "timeout_ms": 5000})
    if not r.get('ok'):
        print(f"  CALL {method} failed: {r.get('error')!r}")
    return r.get('response') or {}


def start(cid, values, run):
    r = post('start_challenge', {"instance": 0, "input": {"instance": 0, "challenge_id": cid, "values": values, "run_id": run, "seed": 7, "timeout_ms": 120000}})
    ok = bool(r.get('ok'))
    print(f"START {cid} {values}: ok={ok} err={r.get('error')!r} {r.get('elapsed_ms')}ms")
    if ok:
        print("  task:", r['response'].get('instruction', '').split('Task: ', 1)[-1].split('\n')[0][:160])
    else:
        failures.append(f"start {cid}: {r.get('error')}")
    return ok


def stop(reason='e2e'):
    post('stop_challenge', {"instance": 0, "reason": reason})


def tp(x, y, z, yaw=0):
    call('TeleportPlayer', {"position": {"x": x, "y": y, "z": z}, "yaw_deg": yaw})
    time.sleep(1.2)


def seq():
    return max([r['seq'] for r in post('reports', {"since_seq": 0})] + [0])


def wait_completed(since, wait, run, expect):
    deadline = time.time() + wait
    while time.time() < deadline:
        for rep in post('reports', {"since_seq": since}):
            since = max(since, rep['seq'])
            if rep['kind'] == 'challenge_completed' and rep['body'].get('runId') == run:
                b = rep['body']
                ok = b['outcome'] == expect
                print(f"  {'OK ' if ok else 'BAD'} {b['challengeId']} {b['outcome']} score={round(b.get('score', 0), 3)} :: {b.get('detail')!r}")
                if not ok:
                    failures.append(f"{run}: expected {expect}, got {b['outcome']}")
                return since, b
        time.sleep(0.5)
    print(f"  BAD no completion for {run} within {wait}s")
    failures.append(f"{run}: no completion")
    return since, None


def main():
    global API
    ap = argparse.ArgumentParser()
    ap.add_argument('--api', default=API)
    API = ap.parse_args().api

    st = post('state', {})
    print("SDK", st['status']['sdk_version'], "build", st['status']['build_id'], "challenges", [c['id'] for c in st['init']['challenges']])
    inst = (st['status'].get('instances') or [{}])[0]
    print("frames_submitted", inst.get('frames_submitted'), "fps", round(inst.get('fps') or 0))
    if not inst.get('frames_submitted'):
        failures.append("no frames submitted")
    s = seq()

    # complete-mission on hard: objectives listed, then die -> FAILURE
    if start('complete-mission', {"mission": "newjob", "difficulty": "hard", "minutes": 10}, 'e2e-cm'):
        ms = call('GetMissionState')
        print(f"  mission {ms.get('mission')} difficulty {ms.get('difficulty', 0)} loot_total {ms.get('lootTotal')} objectives {len(ms.get('objectives', []))}")
        if ms.get('difficulty', 0) != 2:
            failures.append("difficulty not applied")
        call('ExecConsoleCommand', {"command": "kill"})
        s, _ = wait_completed(s, 30, 'e2e-cm', 'OUTCOME_FAILURE')

    # objective: teleport into the tavern's doorway volume
    if start('newjob-objective', {"objective": "enter-tavern", "difficulty": "easy", "minutes": 10}, 'e2e-obj'):
        tp(1074, -756, 20)
        s, _ = wait_completed(s, 15, 'e2e-obj', 'OUTCOME_SUCCESS')

    # reach on both maps, ghost rule on the second
    if start('newjob-reach', {"location": "kitchen", "stealth": "any", "difficulty": "easy", "minutes": 10}, 'e2e-reach1'):
        tp(1160.5, -101.875, -128.25)
        s, _ = wait_completed(s, 15, 'e2e-reach1', 'OUTCOME_SUCCESS')
    if start('stlucia-reach', {"location": "church_lucia", "stealth": "ghost", "difficulty": "medium", "minutes": 10}, 'e2e-reach2'):
        tp(358, 612, 64)
        s, _ = wait_completed(s, 15, 'e2e-reach2', 'OUTCOME_SUCCESS')

    # steal-loot: totals then stop -> ABORTED
    if start('steal-loot', {"mission": "stlucia", "percent": 10, "stealth": "any", "difficulty": "easy", "minutes": 10}, 'e2e-loot'):
        ms = call('GetMissionState')
        print(f"  loot {ms.get('lootFound', 0)}/{ms.get('lootTotal')}")
        if not ms.get('lootTotal'):
            failures.append("no loot total")
        stop()
        s, _ = wait_completed(s, 10, 'e2e-loot', 'OUTCOME_ABORTED')

    # knockout: start and stop (the kill/KO paths need a real fight)
    if start('knockout', {"mission": "newjob", "count": 1, "difficulty": "easy", "minutes": 10}, 'e2e-ko'):
        stop()
        s, _ = wait_completed(s, 10, 'e2e-ko', 'OUTCOME_ABORTED')

    # explore: three areas, stop, check the final metric
    if start('explore', {"mission": "stlucia", "difficulty": "easy", "minutes": 5}, 'e2e-explore'):
        tp(1984, 4288, -384); tp(734, 612, 64); tp(-2321, 503, 15)
        stop()
        s, b = wait_completed(s, 10, 'e2e-explore', 'OUTCOME_ABORTED')
        if b:
            visited = [m['value'] for m in b.get('finalMetrics', []) if m['name'] == 'explore/locations_visited']
            print("  explore/locations_visited =", visited)
            if not visited or visited[0] < 3:
                failures.append(f"explore visited {visited}")

    kinds = {}
    for rep in post('reports', {"since_seq": 0}):
        kinds[rep['kind']] = kinds.get(rep['kind'], 0) + 1
    print("report kinds:", kinds)
    st = post('state', {})['status']
    inst = (st.get('instances') or [{}])[0]
    print("frames_submitted", inst.get('frames_submitted'), "fps", round(inst.get('fps') or 0), "active", inst.get('active_challenge'))
    print("FAILURES:", failures if failures else "none")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
