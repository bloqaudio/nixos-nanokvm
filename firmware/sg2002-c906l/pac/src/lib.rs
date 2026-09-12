//! Typed SG2002 peripheral register access.
//!
//! Register layouts in this crate are hand-audited against the SG2002 TRM.
//! SoC instance addresses and ownership policy belong to the generated board
//! contract rather than this crate.

#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

pub mod timer;
