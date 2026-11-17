//! Fixed-width fields and the declaration macro of the plain messages.
//!
//! A message whose fields are all fixed-width integers, raw floats,
//! vectors and quaternions is declared with `wire_message!`: the fields in
//! wire order, each with its type. A float the C++ encoder moves through
//! the x87 stack (`FRpcWireStream::PutFloat`) is marked `=> x87`; its
//! decoder copies the bits, as every C++ decoder does.

use super::{WireError, WireReader, WireWriter};

/// A fixed-width field copied as raw little-endian bytes.
pub trait Field: Sized {
    /// Writes the field.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the writer's limit is passed.
    fn put(&self, writer: &mut WireWriter) -> Result<(), WireError>;

    /// Reads the field.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input ends inside it.
    fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError>;
}

macro_rules! scalar_field {
    ($($ty:ty => $put:ident, $get:ident;)*) => {
        $(
            impl Field for $ty {
                fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
                    writer.$put(*self)
                }
                fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
                    reader.$get()
                }
            }
        )*
    };
}

scalar_field! {
    u8 => put_u8, get_u8;
    i8 => put_i8, get_i8;
    u16 => put_u16, get_u16;
    i16 => put_i16, get_i16;
    u32 => put_u32, get_u32;
    i32 => put_i32, get_i32;
    f32 => put_f32, get_f32;
    f64 => put_f64, get_f64;
    [f32; 3] => put_vector, get_vector;
    [f32; 4] => put_quaternion, get_quaternion;
}

impl Field for [u8; 4] {
    fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_bytes(self)
    }
    fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let bytes = reader.get_bytes(4)?;
        let mut out = [0; 4];
        out.copy_from_slice(bytes);
        Ok(out)
    }
}

/// Writes one field, raw or through the x87 stack.
macro_rules! wire_put {
    (; $writer:expr, $value:expr) => {
        $crate::protocol::field::Field::put($value, $writer)?
    };
    (x87; $writer:expr, $value:expr) => {
        $writer.put_f32_x87(*$value)?
    };
    (x87d; $writer:expr, $value:expr) => {
        $writer.put_f64_x87(*$value)?
    };
}

/// Reads one field; the decoders copy bits whatever the encoder did.
macro_rules! wire_get {
    ($($codec:ident)?; $reader:expr) => {
        $crate::protocol::field::Field::get($reader)?
    };
}

/// Declares a message of fixed-width fields and its [`Body`] codec.
///
/// [`Body`]: crate::protocol::Body
macro_rules! wire_message {
    (
        $(#[$meta:meta])*
        pub struct $name:ident = $id:literal, $cname:literal {
            $( $(#[$fmeta:meta])* pub $field:ident : $ty:ty $(=> $codec:ident)? ),* $(,)?
        }
    ) => {
        $(#[$meta])*
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct $name {
            $( $(#[$fmeta])* pub $field: $ty, )*
        }

        impl $crate::protocol::Body for $name {
            const ID: $crate::protocol::PacketId = $id;
            const NAME: &'static str = $cname;

            #[allow(unused_variables)]
            fn encode_body(
                &self,
                ctx: &$crate::protocol::WireContext,
                writer: &mut $crate::protocol::WireWriter,
            ) -> Result<(), $crate::protocol::WireError> {
                $( wire_put!($($codec)?; writer, &self.$field); )*
                Ok(())
            }

            #[allow(unused_variables)]
            fn decode_body(
                ctx: &$crate::protocol::WireContext,
                reader: &mut $crate::protocol::WireReader<'_>,
            ) -> Result<Self, $crate::protocol::WireError> {
                Ok(Self { $( $field: wire_get!($($codec)?; reader), )* })
            }
        }
    };
}

/// Declares a message that is one `u32` size and that many bytes
/// (`FRpcCommonBufferPacketBase`, `FRpcServerBlobPacket`).
macro_rules! buffer_message {
    ($(#[$meta:meta])* pub struct $name:ident = $id:literal, $cname:literal;) => {
        $(#[$meta])*
        #[derive(Clone, Debug, Default, PartialEq, Eq)]
        pub struct $name {
            /// The bytes.
            pub data: Vec<u8>,
        }

        impl $crate::protocol::Body for $name {
            const ID: $crate::protocol::PacketId = $id;
            const NAME: &'static str = $cname;

            fn encode_body(
                &self,
                _ctx: &$crate::protocol::WireContext,
                writer: &mut $crate::protocol::WireWriter,
            ) -> Result<(), $crate::protocol::WireError> {
                writer.put_bytes_u32(&self.data)
            }

            fn decode_body(
                _ctx: &$crate::protocol::WireContext,
                reader: &mut $crate::protocol::WireReader<'_>,
            ) -> Result<Self, $crate::protocol::WireError> {
                Ok(Self {
                    data: reader.get_bytes_u32()?.to_vec(),
                })
            }
        }
    };
}

/// Declares a channel's message enum: one variant per message struct, of
/// the same name, with the header dispatch.
macro_rules! catalog {
    ($(#[$meta:meta])* pub enum $name:ident { $( $variant:ident ),* $(,)? }) => {
        $(#[$meta])*
        #[derive(Clone, Debug, PartialEq)]
        pub enum $name {
            $(
                #[doc = concat!("`", stringify!($variant), "`.")]
                $variant($variant),
            )*
        }

        impl $name {
            /// The packet IDs of the channel, in declaration order.
            pub const IDS: &'static [$crate::protocol::PacketId] =
                &[$( <$variant as $crate::protocol::Body>::ID, )*];

            /// The C++ names, in the order of [`Self::IDS`].
            pub const NAMES: &'static [&'static str] =
                &[$( <$variant as $crate::protocol::Body>::NAME, )*];

            /// The packet ID.
            #[must_use]
            pub const fn id(&self) -> $crate::protocol::PacketId {
                match self {
                    $( Self::$variant(_) => <$variant as $crate::protocol::Body>::ID, )*
                }
            }

            /// The C++ name.
            #[must_use]
            pub const fn name(&self) -> &'static str {
                match self {
                    $( Self::$variant(_) => <$variant as $crate::protocol::Body>::NAME, )*
                }
            }

            /// Writes the header and the fields.
            ///
            /// # Errors
            /// As [`Body::encode_body`](crate::protocol::Body::encode_body).
            pub fn encode(
                &self,
                ctx: &$crate::protocol::WireContext,
                writer: &mut $crate::protocol::WireWriter,
            ) -> Result<(), $crate::protocol::WireError> {
                match self {
                    $( Self::$variant(message) => $crate::protocol::Body::encode(message, ctx, writer), )*
                }
            }

            /// The message's bytes, header included.
            ///
            /// # Errors
            /// As [`Body::encode_body`](crate::protocol::Body::encode_body).
            pub fn to_wire(
                &self,
                ctx: &$crate::protocol::WireContext,
            ) -> Result<Vec<u8>, $crate::protocol::WireError> {
                let mut writer = $crate::protocol::WireWriter::new();
                self.encode(ctx, &mut writer)?;
                Ok(writer.into_bytes())
            }

            /// Reads one message, header first.
            ///
            /// # Errors
            /// [`WireError::UnknownMessage`](crate::protocol::WireError::UnknownMessage)
            /// for a header the channel does not have, otherwise as
            /// [`Body::decode_body`](crate::protocol::Body::decode_body).
            pub fn decode(
                ctx: &$crate::protocol::WireContext,
                reader: &mut $crate::protocol::WireReader<'_>,
            ) -> Result<Self, $crate::protocol::WireError> {
                let header = reader.get_header()?;
                let id = header.packet_id();
                $(
                    if id == <$variant as $crate::protocol::Body>::ID {
                        return <$variant as $crate::protocol::Body>::decode_body(ctx, reader)
                            .map(Self::$variant);
                    }
                )*
                Err($crate::protocol::WireError::UnknownMessage(header))
            }

            /// The message `bytes` hold, which must be all of them.
            ///
            /// # Errors
            /// As [`Self::decode`], and
            /// [`WireError::TrailingBytes`](crate::protocol::WireError::TrailingBytes)
            /// when the message ends early.
            pub fn from_wire(
                ctx: &$crate::protocol::WireContext,
                bytes: &[u8],
            ) -> Result<Self, $crate::protocol::WireError> {
                let mut reader = $crate::protocol::WireReader::new(bytes);
                let message = Self::decode(ctx, &mut reader)?;
                reader.finish()?;
                Ok(message)
            }
        }

        $(
            impl From<$variant> for $name {
                fn from(message: $variant) -> Self {
                    Self::$variant(message)
                }
            }
        )*
    };
}

/// Declares the one-field messages.
macro_rules! scalar_messages {
    ($($(#[$meta:meta])* $name:ident = $id:literal, $cname:literal, $ty:ty $(=> $codec:ident)?;)*) => {
        $(
            wire_message! {
                $(#[$meta])*
                pub struct $name = $id, $cname {
                    /// The value.
                    pub value: $ty $(=> $codec)?,
                }
            }
        )*
    };
}

/// Declares the messages without fields.
macro_rules! empty_messages {
    ($($(#[$meta:meta])* $name:ident = $id:literal, $cname:literal;)*) => {
        $(
            wire_message! {
                $(#[$meta])*
                pub struct $name = $id, $cname {}
            }
        )*
    };
}

/// Declares the equipment-list messages.
macro_rules! equip_list_messages {
    ($($(#[$meta:meta])* $name:ident = $id:literal, $cname:literal;)*) => {
        $(
            $(#[$meta])*
            #[derive(Clone, Debug, Default, PartialEq)]
            pub struct $name {
                /// The records, at most 65 535.
                pub records: Vec<$crate::protocol::records::EquipRecord>,
            }

            impl $crate::protocol::Body for $name {
                const ID: u32 = $id;
                const NAME: &'static str = $cname;

                fn encode_body(&self, _ctx: &$crate::protocol::WireContext, writer: &mut $crate::protocol::WireWriter) -> Result<(), $crate::protocol::WireError> {
                    $crate::protocol::records::EquipRecord::put_list(writer, &self.records)
                }

                fn decode_body(_ctx: &$crate::protocol::WireContext, reader: &mut $crate::protocol::WireReader<'_>) -> Result<Self, $crate::protocol::WireError> {
                    Ok(Self { records: $crate::protocol::records::EquipRecord::get_list(reader)? })
                }
            }
        )*
    };
}

/// A count that must fit `max` (the C++ truncates it and writes every
/// element; divergence D98).
pub fn count_fits(length: usize, max: usize) -> Result<usize, WireError> {
    if length > max {
        Err(WireError::LengthOutOfRange { length, max })
    } else {
        Ok(length)
    }
}

/// `length` as a `u8` count.
pub fn count_u8(length: usize) -> Result<u8, WireError> {
    u8::try_from(length).map_err(|_| WireError::LengthOutOfRange {
        length,
        max: usize::from(u8::MAX),
    })
}

/// `length` as a `u16` count.
pub fn count_u16(length: usize) -> Result<u16, WireError> {
    u16::try_from(length).map_err(|_| WireError::LengthOutOfRange {
        length,
        max: usize::from(u16::MAX),
    })
}

/// `length` as a `u32` count.
pub fn count_u32(length: usize) -> Result<u32, WireError> {
    u32::try_from(length).map_err(|_| WireError::LengthOutOfRange {
        length,
        max: usize::try_from(u32::MAX).unwrap_or(usize::MAX),
    })
}

/// A `u32` count from the wire as a length; a count the input cannot hold
/// fails with [`WireError::Underrun`] before anything is allocated.
pub fn wire_len(count: u32, width: usize, reader: &WireReader<'_>) -> Result<usize, WireError> {
    let remaining = reader.remaining();
    usize::try_from(count)
        .ok()
        .and_then(|c| c.checked_mul(width))
        .filter(|bytes| *bytes <= remaining)
        .and_then(|_| usize::try_from(count).ok())
        .ok_or(WireError::Underrun {
            needed: usize::try_from(count)
                .ok()
                .and_then(|c| c.checked_mul(width))
                .unwrap_or(usize::MAX),
            remaining,
        })
}

/// A list of `count` fields of `width` bytes each.
pub fn get_list<'a, T>(
    reader: &mut WireReader<'a>,
    count: u32,
    width: usize,
    mut item: impl FnMut(&mut WireReader<'a>) -> Result<T, WireError>,
) -> Result<Vec<T>, WireError> {
    let count = wire_len(count, width, reader)?;
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        out.push(item(reader)?);
    }
    Ok(out)
}

/// `count` little-endian `u32` values.
pub fn get_u32s(reader: &mut WireReader<'_>, count: u32) -> Result<Vec<u32>, WireError> {
    get_list(reader, count, 4, WireReader::get_u32)
}

/// Every value of `values` as a little-endian `u32`, one field.
pub fn put_u32s(writer: &mut WireWriter, values: &[u32]) -> Result<(), WireError> {
    let bytes: Vec<u8> = values.iter().flat_map(|v| v.to_le_bytes()).collect();
    writer.put_bytes(&bytes)
}

/// Every unit of `units` as two little-endian bytes, one field.
pub fn put_units(writer: &mut WireWriter, units: &[u16]) -> Result<(), WireError> {
    writer.put_wide(units)
}
