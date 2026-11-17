//! Property tests of the protocol codecs (decision T13 allows `proptest`):
//! for every message of the catalog, filled from random seeds in every
//! form, `decode(encode(x)) == x` and the bytes re-encode to themselves;
//! and no input makes a decoder panic.

mod support;

use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::marshal::MarshalReader;
use game_sim::protocol::rdl::{extract_text, read_records, write_records};
use game_sim::protocol::to_server::{Login, TradeResponse, lower_ascii_units};
use game_sim::protocol::{
    Body, ClientMessage, CommonMessage, ServerMessage, ToClient, ToServer, WireContext, WireReader,
};
use proptest::prelude::*;
use support::Gen;
use support::fill::{self, Mode};

const ALL_MODES: [Mode; 3] = [Mode::Client, Mode::Server, Mode::Single];
const SERVER_MODES: [Mode; 2] = [Mode::Server, Mode::Single];

fn normalize_client(message: ClientMessage) -> ClientMessage {
    match message {
        ClientMessage::Login(login) => ClientMessage::Login(Login {
            account_name: lower_ascii_units(&login.account_name),
            ..login
        }),
        other => other,
    }
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(512))]

    #[test]
    fn common_round_trip(index in 0..fill::COMMON.len(), case in 0u32..16, mode in 0..ALL_MODES.len(), seed: u32) {
        let entry = &fill::COMMON[index];
        let mode = ALL_MODES[mode];
        let ctx = mode.context(1);
        let message = fill::common(entry.id, case % entry.cases, mode, &mut Gen::new(seed));
        let bytes = message.to_wire(&ctx).unwrap();
        let back = CommonMessage::from_wire(&ctx, &bytes).unwrap();
        prop_assert_eq!(&back, &message);
        prop_assert_eq!(back.to_wire(&ctx).unwrap(), bytes);
    }

    #[test]
    fn client_round_trip(index in 0..fill::CLIENT.len(), case in 0u32..16, single: bool, seed: u32) {
        let entry = &fill::CLIENT[index];
        let mode = if single { Mode::Single } else { Mode::Client };
        let ctx = mode.context(3);
        let message = fill::client(entry.id, case % entry.cases, &mut Gen::new(seed));
        let bytes = message.to_wire(&ctx).unwrap();
        let back = ClientMessage::from_wire(&ctx, &bytes).unwrap();
        prop_assert_eq!(&back, &normalize_client(message));
        prop_assert_eq!(back.to_wire(&ctx).unwrap(), bytes);
    }

    #[test]
    fn trade_response_round_trip(case in 0u32..2, seed: u32) {
        let message = fill::client(TradeResponse::ID, case, &mut Gen::new(seed));
        let bytes = message.to_wire(&WireContext::CLIENT).unwrap();
        prop_assert_eq!(ClientMessage::from_wire(&WireContext::CLIENT, &bytes).unwrap(), message);
    }

    #[test]
    fn server_round_trip(index in 0..fill::SERVER.len(), case in 0u32..16, mode in 0..SERVER_MODES.len(), seed: u32) {
        let entry = &fill::SERVER[index];
        let mode = SERVER_MODES[mode];
        let ctx = mode.context(2);
        let message = fill::server(entry.id, case % entry.cases, mode, &mut Gen::new(seed));
        let bytes = message.to_wire(&ctx).unwrap();
        let back = ServerMessage::from_wire(&ctx, &bytes).unwrap();
        prop_assert_eq!(&back, &message);
        prop_assert_eq!(back.to_wire(&ctx).unwrap(), bytes);
    }

    #[test]
    fn decoders_do_not_panic(bytes in proptest::collection::vec(any::<u8>(), 0..300), single: bool) {
        for ctx in [
            if single { WireContext::SINGLE_PLAYER_SERVER } else { WireContext::SERVER },
            if single { WireContext::SINGLE_PLAYER_CLIENT } else { WireContext::CLIENT },
        ] {
            let _ = ToClient::decode(&ctx, &mut WireReader::new(&bytes));
            let _ = ToServer::decode(&ctx, &mut WireReader::new(&bytes));
        }
        let _ = FmtStr::unflatten(&bytes);
        let (records, _) = read_records(&bytes);
        let _ = write_records(&records, 0x1000);
        let _ = extract_text(&bytes, 16);
        let mut reader = MarshalReader::new(&bytes);
        let _ = reader.get_fmt_str();
        let _ = reader.get_equip_desc_list();
        let _ = reader.get_good_desc_list();
        let _ = reader.get_costume();
        let _ = reader.get_wstring();
    }

    #[test]
    fn every_header_dispatches(channel in 1u8..4, opcode: u8, tail in proptest::collection::vec(any::<u8>(), 0..64)) {
        let mut bytes = vec![opcode, channel];
        bytes.extend_from_slice(&tail);
        let _ = CommonMessage::from_wire(&WireContext::CLIENT, &bytes);
        let _ = ServerMessage::from_wire(&WireContext::SERVER, &bytes);
        let _ = ClientMessage::from_wire(&WireContext::CLIENT, &bytes);
    }
}

#[test]
fn catalog_has_162_ids() {
    let mut ids: Vec<u32> = CommonMessage::IDS
        .iter()
        .chain(ServerMessage::IDS)
        .chain(ClientMessage::IDS)
        .copied()
        .collect();
    assert_eq!(ids.len(), 162);
    ids.sort_unstable();
    ids.dedup();
    assert_eq!(ids.len(), 162);
    for id in ids {
        let [opcode, _, channel, _] = id.to_le_bytes();
        let header = game_sim::protocol::MessageHeader::new(channel, opcode);
        assert_eq!(header.packet_id(), id);
    }
}

#[test]
fn every_catalog_entry_has_a_suite_line() {
    // Every packet but TRADERESPONSE has oracle vectors.
    let suites = fill::COMMON.len() + fill::CLIENT.len() + fill::SERVER.len();
    assert_eq!(suites, 162 - 1);
}
