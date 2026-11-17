//! Dump lines of the fuse files and the petal database
//! (`CppSource/Oracle/oracle_game_universe.cpp` writes the same).

use game_sim::data::fuse::{FuseFiles, PetalDb};

use super::render::{Line, Render, json_text};

impl Render<'_> {
    /// Kind `fuses`, file `fuse_files`: the recorded paths, relative to the
    /// data root.
    pub fn fuse_files(files: &FuseFiles) -> Vec<String> {
        files
            .0
            .iter()
            .map(|p| Line::new().add("fuse_file", json_text(p)).done())
            .collect()
    }

    /// Kind `fuses`, file `petals`: every record, categories in order,
    /// nickname ids in order.
    pub fn petals(&self, db: &PetalDb) -> Vec<String> {
        let mut lines = Vec::new();
        for (category, records) in db.categories.iter().enumerate() {
            for r in records.values() {
                let extras: Vec<String> = r.extras.iter().map(|&e| self.text(e)).collect();
                lines.push(
                    Line::new()
                        .add("category", category.to_string())
                        .add("petal", format!("{:08x}", r.nickname))
                        .add("mesh", json_text(&r.mesh))
                        .add("extras", extras.join(";"))
                        .done(),
                );
            }
        }
        lines
    }
}
