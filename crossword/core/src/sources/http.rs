//! A small blocking HTTP client over `ureq`, with a timeout and error mapping.

use std::time::Duration;

use super::SourceError;

const TIMEOUT: Duration = Duration::from_secs(20);

pub struct Http {
    agent: ureq::Agent,
}

impl Default for Http {
    fn default() -> Self {
        Http::new()
    }
}

impl Http {
    pub fn new() -> Http {
        let agent = ureq::Agent::config_builder()
            .timeout_global(Some(TIMEOUT))
            .user_agent(concat!("crossword/", env!("CARGO_PKG_VERSION")))
            .build()
            .into();
        Http { agent }
    }

    /// Fetches `url` as text. `cookie` is sent as the `Cookie` header.
    pub fn get(&self, url: &str, cookie: Option<&str>) -> Result<String, SourceError> {
        let mut request = self.agent.get(url);
        if let Some(cookie) = cookie {
            request = request.header("Cookie", cookie);
        }
        match request.call() {
            Ok(mut response) => response
                .body_mut()
                .read_to_string()
                .map_err(|e| SourceError::Network(e.to_string())),
            Err(ureq::Error::StatusCode(401 | 403)) => Err(SourceError::Refused),
            Err(ureq::Error::StatusCode(404)) => Err(SourceError::NotFound),
            Err(e) => Err(SourceError::Network(e.to_string())),
        }
    }
}
