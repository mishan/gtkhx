//! RC4 (ARC4) stream cipher implementation.
//!
//! Byte-for-byte compatible with Nettle's arcfour_crypt and the wire
//! protocol's expectations.

/// RC4 state: 256-byte permutation table + two indices.
pub struct Rc4State {
    s: [u8; 256],
    i: u8,
    j: u8,
}

impl Rc4State {
    /// Initialize RC4 with the given key.
    pub fn new(key: &[u8]) -> Self {
        let mut state = Rc4State {
            s: [0u8; 256],
            i: 0,
            j: 0,
        };
        // KSA (Key-Scheduling Algorithm)
        for i in 0..256u16 {
            state.s[i as usize] = i as u8;
        }
        let mut j: u8 = 0;
        for i in 0..256u16 {
            j = j
                .wrapping_add(state.s[i as usize])
                .wrapping_add(key[(i as usize) % key.len()]);
            state.s.swap(i as usize, j as usize);
        }
        state
    }

    /// Re-key the state (reinitialize with a new key).
    /// Resets the stream position.
    pub fn set_key(&mut self, key: &[u8]) {
        *self = Self::new(key);
    }

    /// Encrypt or decrypt (XOR with keystream). Safe for in-place (src == dst pointer).
    pub fn crypt(&mut self, src: &[u8], dst: &mut [u8]) {
        debug_assert_eq!(src.len(), dst.len());
        for (s, d) in src.iter().zip(dst.iter_mut()) {
            self.i = self.i.wrapping_add(1);
            self.j = self.j.wrapping_add(self.s[self.i as usize]);
            self.s.swap(self.i as usize, self.j as usize);
            let k = self.s[self.s[self.i as usize].wrapping_add(self.s[self.j as usize]) as usize];
            *d = s ^ k;
        }
    }
}
