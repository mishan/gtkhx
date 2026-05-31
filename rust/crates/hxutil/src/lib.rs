/// Placeholder crate to validate the Cargo workspace builds correctly.
/// Real utility code will land here in later phases.

#[cfg(test)]
mod tests {
    #[test]
    fn workspace_builds() {
        // If this compiles and runs, the Cargo workspace is wired up correctly.
        assert_eq!(2 + 2, 4);
    }
}
