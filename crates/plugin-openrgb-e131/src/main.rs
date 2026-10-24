//! OpenRGB (E1.31), a Foxglove plugin: OpenRGB, or any sACN/E1.31 sender, colours every
//! keyboard's lights. Point an OpenRGB E1.31 device at 127.0.0.1, universe 1, and its colour
//! becomes the keyboards' underglow colour (the average of the LEDs it sends).
//!
//! - Receives on 127.0.0.1:5568 (UDP) only, through Foxglove (`net.listen:udp:127.0.0.1:5568`),
//!   so the plugin itself runs sandboxed, without any network of its own.
//! - Sends `lighting.set` only when the colour changes, at most `--max-hz` times a second.
//! - When the sender stops (stream terminated, or 2.5 s without data, E1.31's loss timeout),
//!   it puts each keyboard's lights back as they were.
//!
//! Options (in `plugin.toml`'s `run`): `--universe N` (1), `--start-channel N` (1), `--leds N`
//! (every LED sent), `--max-hz N` (10). `--send-test R G B [--seq N] [--stop]` sends one frame
//! to the plugin and exits, for testing without OpenRGB.

mod e131;

use std::net::{Ipv4Addr, UdpSocket};
use std::time::{Duration, Instant};

use foxglove_plugin::{Hsb, LightingState, NetEvent, Plugin, SetLightingParams};

/// E1.31 6.7.1: a stream is lost after 2.5 s without data.
const LOSS_TIMEOUT: Duration = Duration::from_millis(2500);

struct Options {
    universe: u16,
    start_channel: usize,
    leds: Option<usize>,
    max_hz: u32,
}

fn options(args: &[String]) -> Options {
    let num = |flag: &str| -> Option<u64> { args.windows(2).find(|w| w[0] == flag).and_then(|w| w[1].parse().ok()) };
    Options {
        universe: num("--universe").map_or(1, |n| n as u16),
        start_channel: num("--start-channel").map_or(1, |n| (n as usize).max(1)),
        leds: num("--leds").map(|n| n as usize),
        max_hz: num("--max-hz").map_or(10, |n| (n as u32).clamp(1, 30)),
    }
}

/// The average colour of the RGB triples from `start_channel` (1-based), `leds` of them or
/// all there are; `None` with no whole LED.
fn average(channels: &[u8], start_channel: usize, leds: Option<usize>) -> Option<(u8, u8, u8)> {
    let rgb = channels.get(start_channel - 1..)?;
    let n = leds.map_or(rgb.len() / 3, |l| l.min(rgb.len() / 3));
    if n == 0 {
        return None;
    }
    let sum = rgb.as_chunks::<3>().0.iter().take(n).fold([0u32; 3], |s, c| [s[0] + u32::from(c[0]), s[1] + u32::from(c[1]), s[2] + u32::from(c[2])]);
    let avg = |v: u32| (v / n as u32) as u8;
    Some((avg(sum[0]), avg(sum[1]), avg(sum[2])))
}

/// RGB to the device layer's HSB (hue 0-359, saturation and brightness 0-100).
fn hsb((r, g, b): (u8, u8, u8)) -> Hsb {
    let (r, g, b) = (f32::from(r) / 255.0, f32::from(g) / 255.0, f32::from(b) / 255.0);
    let max = r.max(g).max(b);
    let delta = max - r.min(g).min(b);
    let hue = if delta == 0.0 {
        0.0
    } else if max == r {
        60.0 * ((g - b) / delta).rem_euclid(6.0)
    } else if max == g {
        60.0 * ((b - r) / delta + 2.0)
    } else {
        60.0 * ((r - g) / delta + 4.0)
    };
    let saturation = if max == 0.0 { 0.0 } else { delta / max };
    Hsb { hue: (hue.round() as u16) % 360, saturation: (saturation * 100.0).round() as u8, brightness: (max * 100.0).round() as u8 }
}

/// The keyboards with lights, and how their lights were before we took over.
fn take_over(plugin: &Plugin) -> Vec<(String, Option<LightingState>)> {
    plugin.devices()
        .unwrap_or_default()
        .into_iter()
        .filter(|d| d.connected && d.caps.iter().any(|c| c == "lighting"))
        .map(|d| {
            let before = plugin.lighting_state(&d.id).ok();
            (d.id, before)
        })
        .collect()
}

/// Sends only what changed since `previous` (each part is a separate keyboard command): the
/// first colour also turns the lights on and picks the static effect.
fn colour(plugin: &Plugin, devices: &[(String, Option<LightingState>)], color: Hsb, previous: Option<Hsb>) {
    let lit = |c: Hsb| c.brightness > 0;
    let on = previous.is_none_or(|p| lit(p) != lit(color)).then_some(lit(color));
    // Effect 0 is the static colour.
    let effect = previous.is_none().then(|| "0".to_string());
    for (id, _) in devices {
        let set = SetLightingParams { device: id.clone(), on, color: Some(color), effect: effect.clone() };
        if let Err(e) = plugin.set_lighting(&set) {
            eprintln!("openrgb-e131: lighting: {e}");
            continue;
        }
        let all = SetLightingParams { device: id.clone(), on: Some(color.brightness > 0), color: Some(color), effect: Some("0".into()) };
        confirm(plugin, &all);
    }
}

/// How often a colour the keyboard didn't take is sent again.
const RESENDS: u32 = 3;

/// Whether the keyboard shows what `set` asked for: on or off, and the hue and saturation
/// (brightness aside: the keyboard may dim on battery).
fn shows(state: &LightingState, set: &SetLightingParams) -> bool {
    let near = |a: u16, b: u16, wrap: u16| {
        let d = a.abs_diff(b);
        d.min(wrap.saturating_sub(d)) <= 2
    };
    set.on.is_none_or(|on| state.on == on)
        && (!state.on
            || set.color.is_none_or(|c| near(state.hue, c.hue, 360) && near(state.saturation.into(), c.saturation.into(), 1000)))
}

/// Reads the lights back after a change and sends the whole colour again (on, colour, static
/// effect) if the keyboard didn't take it, a few times at most: a write can be lost while the
/// keyboard is busy with the previous one, and it wouldn't be retried otherwise, since only
/// changes are sent.
fn confirm(plugin: &Plugin, all: &SetLightingParams) {
    let id = &all.device;
    for attempt in 0..=RESENDS {
        match plugin.lighting_state(id) {
            Ok(state) if shows(&state, all) => return,
            Ok(_) if attempt < RESENDS => {
                std::thread::sleep(std::time::Duration::from_millis(100));
                if let Err(e) = plugin.set_lighting(all) {
                    eprintln!("openrgb-e131: lighting: {e}");
                    return;
                }
            }
            Ok(state) => {
                eprintln!("openrgb-e131: {id} still shows {state:?} after {RESENDS} resends");
                return;
            }
            Err(_) => return, // can't read it back; the write itself succeeded
        }
    }
}

fn restore(plugin: &Plugin, devices: &[(String, Option<LightingState>)]) {
    for (id, before) in devices {
        let Some(s) = before else { continue };
        let set = SetLightingParams {
            device: id.clone(),
            on: Some(s.on),
            color: Some(Hsb { hue: s.hue, saturation: s.saturation, brightness: s.brightness }),
            effect: Some(s.effect.clone()),
        };
        match plugin.set_lighting(&set) {
            Ok(()) => confirm(plugin, &set),
            Err(e) => eprintln!("openrgb-e131: restoring lights: {e}"),
        }
    }
}

fn send_test(args: &[String], universe: u16) -> std::io::Result<()> {
    let at = args.iter().position(|a| a == "--send-test").unwrap_or(0);
    let v: Vec<u8> = args[at + 1..].iter().take(3).filter_map(|a| a.parse().ok()).collect();
    let [r, g, b] = v[..] else {
        eprintln!("openrgb-e131: --send-test R G B");
        std::process::exit(2);
    };
    // --seq N numbers the frame (a repeat of the last number counts as stale); --stop marks it
    // the stream's end.
    let seq = args.windows(2).find(|w| w[0] == "--seq").and_then(|w| w[1].parse().ok()).unwrap_or(1);
    let options = if args.iter().any(|a| a == "--stop") { 0x40 } else { 0 };
    let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0))?;
    let frame: Vec<u8> = [r, g, b].repeat(8);
    socket.send_to(&e131::build(universe, seq, options, &frame), (Ipv4Addr::LOCALHOST, e131::PORT))?;
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let opts = options(&args);
    if args.iter().any(|a| a == "--send-test") {
        if let Err(e) = send_test(&args, opts.universe) {
            eprintln!("openrgb-e131: {e}");
            std::process::exit(1);
        }
        return;
    }
    let plugin = match Plugin::connect("openrgb-e131", env!("CARGO_PKG_VERSION")) {
        Ok(plugin) => plugin,
        Err(e) => {
            eprintln!("openrgb-e131: can't reach Foxglove: {e}");
            std::process::exit(1);
        }
    };
    // Localhost only: other machines can't drive the lights. Foxglove does the listening.
    let frames = match plugin.listen_udp(e131::PORT) {
        Ok(rx) => rx,
        Err(e) => {
            eprintln!("openrgb-e131: can't listen on 127.0.0.1:{}: {e}", e131::PORT);
            std::process::exit(1);
        }
    };
    let period = Duration::from_millis(1000 / u64::from(opts.max_hz));
    let mut last_seq: Option<u8> = None;
    let mut devices: Option<Vec<(String, Option<LightingState>)>> = None;
    let mut last_data = Instant::now();
    let mut wanted: Option<Hsb> = None;
    let mut sent: Option<Hsb> = None;
    let mut next_send = Instant::now();
    let mut next_check = Instant::now() + Duration::from_secs(10);
    loop {
        // Foxglove gone (stopped, or killed): don't linger.
        if plugin.is_closed() {
            return;
        }
        let mut stop = false;
        let received = match frames.recv_timeout(Duration::from_millis(250)) {
            Ok(NetEvent::Data(bytes, from)) => Some((bytes, from.unwrap_or_default())),
            Ok(NetEvent::Closed(why)) => {
                eprintln!("openrgb-e131: Foxglove stopped listening: {why}");
                return;
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => None,
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return,
        };
        if let Some((buf, from)) = received {
            if let Some(p) = e131::parse(&buf).filter(|p| p.universe == opts.universe && !p.preview) {
                if last_seq.is_some_and(|last| e131::is_stale(last, p.sequence)) && !p.terminated {
                    continue;
                }
                last_seq = Some(p.sequence);
                last_data = Instant::now();
                if p.terminated {
                    stop = true;
                } else if let Some(rgb) = average(p.channels, opts.start_channel, opts.leds) {
                    if devices.is_none() {
                        eprintln!("openrgb-e131: first frame from {from} (universe {}): taking over the lights", p.universe);
                        devices = Some(take_over(&plugin));
                    }
                    wanted = Some(hsb(rgb));
                }
            }
        }
        let now = Instant::now();
        if devices.is_some() && (stop || now.duration_since(last_data) > LOSS_TIMEOUT) {
            restore(&plugin, devices.as_deref().unwrap_or_default());
            (devices, wanted, sent, last_seq) = (None, None, None, None);
            continue;
        }
        // Idle on the socket, so check now and then that Foxglove is still there.
        if now >= next_check {
            if plugin.call("ping", serde_json::json!({})).is_err() {
                return;
            }
            next_check = now + Duration::from_secs(10);
        }
        if let (Some(d), Some(color)) = (&devices, wanted) {
            if sent != Some(color) && now >= next_send {
                colour(&plugin, d, color, sent);
                sent = Some(color);
                next_send = now + period;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn averages_the_chosen_leds() {
        let ch = [255, 0, 0, 0, 0, 255, 9, 9, 9];
        assert_eq!(average(&ch, 1, Some(2)), Some((127, 0, 127)));
        assert_eq!(average(&ch, 4, None), Some((4, 4, 132)));
        assert_eq!(average(&ch, 8, None), None);
        assert_eq!(average(&ch, 20, None), None);
    }

    #[test]
    fn rgb_to_hsb() {
        assert_eq!(hsb((255, 0, 0)), Hsb { hue: 0, saturation: 100, brightness: 100 });
        assert_eq!(hsb((0, 255, 0)), Hsb { hue: 120, saturation: 100, brightness: 100 });
        assert_eq!(hsb((0, 0, 128)), Hsb { hue: 240, saturation: 100, brightness: 50 });
        assert_eq!(hsb((255, 0, 255)), Hsb { hue: 300, saturation: 100, brightness: 100 });
        assert_eq!(hsb((0, 0, 0)), Hsb { hue: 0, saturation: 0, brightness: 0 });
        assert_eq!(hsb((255, 255, 255)).saturation, 0);
    }

    #[test]
    fn options_parse_and_clamp() {
        let a: Vec<String> = ["--universe", "4", "--start-channel", "0", "--max-hz", "99"].map(String::from).to_vec();
        let o = options(&a);
        assert_eq!((o.universe, o.start_channel, o.leds, o.max_hz), (4, 1, None, 30));
    }
}
