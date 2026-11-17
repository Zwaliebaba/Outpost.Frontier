use super::channel::{unpack_axis_angle, unpack_quaternion3, unpack_quaternion4};
use super::*;

fn header(frame_count: u32, interval: f32, flags: u32) -> ChannelHeader {
    ChannelHeader {
        frame_count,
        interval,
        flags,
    }
}

fn floats(values: &[f32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn shorts(values: &[i16]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

#[test]
fn constants_have_the_reference_bits() {
    assert_eq!(SHORT_TO_UNIT.to_bits(), 0x3800_0100);
    assert_eq!(core::f32::consts::PI.to_bits(), 0x4049_0FDB);
}

#[test]
fn header_round_trips() {
    let h = header(7, -1.0, 0x142);
    assert_eq!(ChannelHeader::parse(h.to_bytes()), h);
    assert!(h.has_time_stamps());
    assert!(!header(1, -0.0, 0).has_time_stamps());
    assert!(!header(1, f32::NAN, 0).has_time_stamps());
}

#[test]
fn strides_follow_the_reference_sequence() {
    let cases = [
        (0x1, 0.1, 4),
        (0x2, 0.1, 12),
        (0x4, 0.1, 16),
        (0x6, -1.0, 32),
        (0x7, 0.1, 32),
        // Bit 8 resets the sum to 4.
        (0xF, 0.1, 4),
        (0x8, -1.0, 8),
        (0x10, 0.1, 0),
        (0x20, -1.0, 4),
        (0x40, 0.1, 6),
        (0x42, 0.1, 18),
        (0x80, -1.0, 10),
        (0x100, 0.1, 8),
        (0x1C0, 0.1, 20),
        (0x200, 0.1, 0),
    ];
    for (flags, interval, stride) in cases {
        assert_eq!(stride_for(flags, interval), stride, "flags {flags:#x}");
    }
}

#[test]
fn raw_channels_keep_their_bytes() {
    let frames = floats(&[0.0, 1.0, 2.0, 3.0, 0.5, 4.0, 5.0, 6.0]);
    let channel = Channel::decode(header(2, -1.0, 2), frames.clone()).unwrap();
    assert_eq!(channel.frames(), frames.as_slice());
    assert_eq!(channel.stride(), 16);
    assert_eq!(channel.duration(), 0.5);
    assert_eq!(channel.frame_time(1), Some(0.5));
    assert_eq!(channel.kind(), Some(ChannelKind::Vector));
    assert_eq!(
        channel.position(1).map(|v| (v.x, v.y, v.z)),
        Some((4.0, 5.0, 6.0))
    );
    assert_eq!(channel.position(2), None);
    assert_eq!(channel.quaternion(0), None);

    let uniform = Channel::decode(header(3, 0.25, 1), floats(&[1.0, 2.0, 3.0])).unwrap();
    assert_eq!(uniform.duration(), 0.5);
    assert_eq!(uniform.frame_time(2), Some(0.5));
    assert_eq!(uniform.scalar(2), Some(3.0));
}

#[test]
fn size_checks_wrap_like_the_reference() {
    assert_eq!(
        Channel::decode(header(3, 0.1, 2), floats(&[0.0; 6])),
        Err(ChannelError::SizeMismatch {
            size: 24,
            expected: 36
        })
    );
    // 0x4000_0000 frames of 4 bytes wrap to 0.
    assert_eq!(
        Channel::decode(header(0x4000_0000, 0.1, 1), Vec::new()),
        Err(ChannelError::Overflow)
    );
    // Event channels skip the check; a stamped one still needs its stamps.
    assert!(Channel::decode(header(5, 0.1, 8), shorts(&[1, 2])).is_ok());
    assert_eq!(
        Channel::decode(header(5, -1.0, 8), floats(&[0.0, 0.0])),
        Err(ChannelError::StampOutsideFrames)
    );
    assert_eq!(
        Channel::decode(header(0, -1.0, 2), Vec::new()),
        Err(ChannelError::StampOutsideFrames)
    );
}

#[test]
fn event_offsets_point_after_the_frames() {
    let mut frames = floats(&[0.0]);
    frames.extend_from_slice(&3u32.to_le_bytes());
    frames.extend_from_slice(b"abcdef");
    let channel = Channel::decode(header(1, -1.0, 8), frames).unwrap();
    assert_eq!(channel.event_offset(0), Some(8 + 3));
    assert_eq!(&channel.frames()[11..], b"def");
}

#[test]
fn packed_quaternions_rebuild_w() {
    let [w, x, y, z] = unpack_quaternion3(&shorts(&[0, 16384, 0]));
    assert_eq!((x, z), (0.0, 0.0));
    assert_eq!(y.to_bits(), 0x3F00_0100);
    assert_eq!(w.to_bits(), 0x3F5D_B343);
    // |xyz| >= 1 gives w = 0.
    assert_eq!(unpack_quaternion3(&shorts(&[32767, 32767, 0]))[0], 0.0);
}

#[test]
fn axis_angle_zero_is_identity() {
    assert_eq!(unpack_axis_angle(&shorts(&[0, 0, 0])), [1.0, 0.0, 0.0, 0.0]);
    let [w, x, ..] = unpack_axis_angle(&shorts(&[16384, 0, 0]));
    // Half of pi about x: w = cos(pi/4) and x = sin(pi/4), within a few ulps.
    assert!((w - core::f32::consts::FRAC_1_SQRT_2).abs() < 1e-4);
    assert!((x - core::f32::consts::FRAC_1_SQRT_2).abs() < 1e-4);
}

#[test]
fn four_shorts_take_one_newton_step() {
    let q = unpack_quaternion4(&shorts(&[0, 23170, 0, 23170]));
    let norm: f32 = q.iter().map(|v| v * v).sum();
    assert!((norm - 1.0).abs() < 1e-4);
}

#[test]
fn packed_channels_unpack_to_position_and_quaternion() {
    let mut frames = floats(&[0.0, 1.0, 2.0, 3.0]);
    frames.extend(shorts(&[0, 16384, 0]));
    let channel = Channel::decode(header(1, -1.0, 0x42), frames).unwrap();
    assert_eq!(channel.flags(), 6);
    assert_eq!(channel.stride(), 32);
    assert_eq!(channel.kind(), Some(ChannelKind::PositionQuaternion));
    assert_eq!(channel.position(0).map(|v| v.z), Some(3.0));
    assert_eq!(
        channel.quaternion(0).map(|q| q.y.to_bits()),
        Some(0x3F00_0100)
    );
    assert_eq!(channel.fatal(), None);

    let zero = Channel::decode(header(2, 0.5, 0x30), Vec::new()).unwrap();
    assert_eq!(zero.flags(), 6);
    assert_eq!(
        zero.frames(),
        floats(&[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0].repeat(2)).as_slice()
    );
    assert_eq!(zero.duration(), 0.5);
}

#[test]
fn inconsistent_packed_flags_stay_as_read() {
    let cases = [
        (0x41, DecodeFatal::NotVectorOrQuaternion, 10),
        (0x52, DecodeFatal::InconsistentVector, 18),
        (0x44, DecodeFatal::InconsistentQuaternion, 22),
        (0xC0, DecodeFatal::InconsistentQuaternion, 12),
        (0x60, DecodeFatal::InconsistentQuaternion, 6),
    ];
    for (flags, fatal, stride) in cases {
        let frames = vec![7; stride];
        let channel = Channel::decode(header(1, 0.1, flags), frames.clone()).unwrap();
        assert_eq!(channel.fatal(), Some(fatal), "flags {flags:#x}");
        assert_eq!(channel.flags(), flags);
        assert_eq!(channel.frames(), frames.as_slice());
    }
}

#[test]
fn loaded_channel_reports_the_archetype_fields() {
    let failed = LoadedChannel {
        header: Some(header(3, 0.1, 0x40)),
        outcome: Err(ChannelError::SizeMismatch {
            size: 0,
            expected: 18,
        }),
    };
    assert_eq!(failed.result_code(), 0xFFFF_FFFF);
    assert_eq!(failed.flags(), 0x40);
    assert_eq!(failed.stride(), 6);
    assert_eq!(failed.duration(), 0.0);
    let none = LoadedChannel {
        header: None,
        outcome: Err(ChannelError::NoHeader),
    };
    assert_eq!(
        (none.frame_count(), none.stride(), none.result_code()),
        (0, 0, 0xFFFF_FFFA)
    );
}

#[test]
fn decode_never_panics_on_arbitrary_input() {
    let mut state = 0x9E37_79B9_u32;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        state
    };
    for _ in 0..5000 {
        let flags = next() & 0x3FF;
        let count = next() % 9;
        let interval = if next() % 2 == 0 { -1.0 } else { 0.125 };
        let len = (next() % 80) as usize;
        let frames: Vec<u8> = (0..len).map(|_| next() as u8).collect();
        if let Ok(channel) = Channel::decode(header(count, interval, flags), frames) {
            for index in 0..=count {
                let _ = channel.frame_time(index);
                let _ = channel.payload(index);
                let _ = channel.position(index);
                let _ = channel.quaternion(index);
                let _ = channel.scalar(index);
                let _ = channel.event_offset(index);
            }
        }
    }
}
