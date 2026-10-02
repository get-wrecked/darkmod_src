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


def start(cid, values, run, expect_ok=True):
    r = post('start_challenge', {"instance": 0, "input": {"instance": 0, "challenge_id": cid, "values": values, "run_id": run, "seed": 7, "timeout_ms": 120000}})
    ok = bool(r.get('ok'))
    print(f"START {cid} {values}: ok={ok} err={r.get('error')!r} {r.get('elapsed_ms')}ms")
    if ok:
        print("  task:", r['response'].get('instruction', '').split('Task: ', 1)[-1].split('\n')[0][:200])
    if ok != expect_ok:
        failures.append(f"start {cid}: ok={ok}, expected {expect_ok} ({r.get('error')})")
    return ok


def stop(reason='e2e'):
    post('stop_challenge', {"instance": 0, "reason": reason})


def play(events):
    return post('play/input', {"instance": 0, "events": events})


def hold(sec):
    """Keep the debug app's play session alive (it ends after 3 s without traffic)."""
    t = time.time()
    while time.time() - t < sec:
        play([])
        time.sleep(0.25)


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
    bench = st['init'].get('benchmark') or []
    print("benchmark cases", len(bench), "first", [(b.get('challengeId') or b.get('challenge_id')) for b in bench[:5]])
    if len(bench) < 30:
        failures.append(f"benchmark has {len(bench)} cases")
    seen = set()
    for b in bench:
        key = json.dumps(b, sort_keys=True)
        if key in seen:
            failures.append(f"duplicate benchmark case {key}")
        seen.add(key)

    # invalid requests are rejected up front
    start('stay-hidden', {"mission": "newjob", "start": "inside", "stealth": "any", "minutes": 1}, 'e2e-bad1', expect_ok=False)
    start('newjob-reach', {"location": "inn", "start": "entrance", "minutes": 5}, 'e2e-bad2', expect_ok=False)
    start('stlucia-item', {"item": "no-such-item", "minutes": 5}, 'e2e-bad3', expect_ok=False)

    # complete-mission on hard with kills forbidden: objectives listed, then die -> FAILURE
    if start('complete-mission', {"mission": "newjob", "difficulty": "hard", "kills": "forbidden", "stealth": "unseen", "minutes": 10}, 'e2e-cm'):
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

    # reach with a start tier: the player is placed at the tier's info_location, not the mission start
    if start('newjob-reach', {"location": "kitchen", "start": "inside", "stealth": "any", "difficulty": "easy", "minutes": 10}, 'e2e-reach3'):
        ps = call('GetPlayerState')
        here = ps.get('location', '')
        print(f"  start=inside: player location {here!r} at {ps.get('position')}")
        if here != 'room_3':
            failures.append(f"start tier placed the player in {here!r}, expected room_3")
        tp(1160.5, -101.875, -128.25)
        s, _ = wait_completed(s, 15, 'e2e-reach3', 'OUTCOME_SUCCESS')

    # item: give the player the guard's key through the test hook
    if start('stlucia-item', {"item": "church-entrance-key", "stealth": "any", "difficulty": "easy", "minutes": 10}, 'e2e-item'):
        call('ExecConsoleCommand', {"command": "arcade_give Key_church_entrance"})
        s, _ = wait_completed(s, 15, 'e2e-item', 'OUTCOME_SUCCESS')

    # unlock: a door (frob mover) and the cash box (frob lock)
    if start('stlucia-unlock', {"door": "vestibule-door", "stealth": "any", "difficulty": "easy", "minutes": 10}, 'e2e-unlock1'):
        time.sleep(1.0)
        call('ExecConsoleCommand', {"command": "arcade_unlock atdm_mover_door_4"})
        s, _ = wait_completed(s, 15, 'e2e-unlock1', 'OUTCOME_SUCCESS')
    if start('newjob-unlock', {"door": "cash-box", "stealth": "any", "difficulty": "easy", "minutes": 10}, 'e2e-unlock2'):
        time.sleep(1.0)
        call('ExecConsoleCommand', {"command": "arcade_unlock JewelleryBoxBody_1"})
        s, _ = wait_completed(s, 15, 'e2e-unlock2', 'OUTCOME_SUCCESS')

    # pickpocket: start and stop (needs a live guard to lift from)
    if start('pickpocket', {"mission": "stlucia", "count": 2, "stealth": "unseen", "difficulty": "easy", "minutes": 10}, 'e2e-pp'):
        stop()
        s, _ = wait_completed(s, 10, 'e2e-pp', 'OUTCOME_ABORTED')

    # stay-hidden for one minute at the tavern entrance -> SUCCESS at the time limit
    if start('stay-hidden', {"mission": "newjob", "start": "entrance", "stealth": "unseen", "difficulty": "easy", "minutes": 1}, 'e2e-hide'):
        s, _ = wait_completed(s, 80, 'e2e-hide', 'OUTCOME_SUCCESS')

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

    # input path: drive the instance through the SDK and check the game reacts
    if start('explore', {"mission": "newjob", "difficulty": "easy", "minutes": 3}, 'e2e-input'):
        post('play/drive', {"instance": 0, "take_over": True})
        hold(0.5)
        st0 = call('GetPlayerState')
        play([{"type": "Key", "code": "KeyW", "down": True}]); hold(1.5); play([{"type": "Key", "code": "KeyW", "down": False}])
        play([{"type": "Move", "dx": 300, "dy": 0}]); hold(0.5)
        st1 = call('GetPlayerState')
        moved = ((st1['position']['x'] - st0['position']['x']) ** 2 + (st1['position']['y'] - st0['position']['y']) ** 2) ** 0.5
        turned = abs(st1['view']['yawDeg'] - st0['view']['yawDeg'])
        print(f"  input: W moved {moved:.1f} units, mouse turned {turned:.1f} deg")
        if moved < 10:
            failures.append(f"injected W did not move the player ({moved:.1f} units)")
        if turned < 5:
            failures.append(f"injected mouse motion did not turn the view ({turned:.1f} deg)")
        # non-gameplay keys are dropped: Escape must not open the menu (the attempt keeps running)
        play([{"type": "Key", "code": "Escape", "down": True}]); hold(0.3); play([{"type": "Key", "code": "Escape", "down": False}]); hold(1.0)
        play([{"type": "Move", "dx": 600, "dy": 0}]); hold(0.3)   # turn around: the first walk may have ended at a wall
        st1 = call('GetPlayerState')
        play([{"type": "Key", "code": "KeyW", "down": True}]); hold(1.0); play([{"type": "Key", "code": "KeyW", "down": False}])
        st2 = call('GetPlayerState')
        moved2 = ((st2['position']['x'] - st1['position']['x']) ** 2 + (st2['position']['y'] - st1['position']['y']) ** 2) ** 0.5
        print(f"  after Escape: W moved {moved2:.1f} units (game still in play)")
        if moved2 < 10:
            failures.append(f"after an injected Escape the player no longer moves ({moved2:.1f} units): menu opened?")
        post('play/release', {"instance": 0})
        stop()
        s, _ = wait_completed(s, 10, 'e2e-input', 'OUTCOME_ABORTED')

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
