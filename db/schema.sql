-- MultiServer Phase 2 MySQL 8.4 스키마
-- 영속 범위: 계정(accounts), 로그인 인증(PBKDF2-HMAC-SHA256), 접속/종료 이력(connection_logs)

CREATE DATABASE IF NOT EXISTS multiserver
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_0900_as_cs;

USE multiserver;

-- 1. 계정 및 인증 정보 (비밀번호 평문 저장 금지: 16B 랜덤 솔트 + 32B PBKDF2-HMAC-SHA256)
CREATE TABLE IF NOT EXISTS accounts (
  account_id     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username       VARCHAR(32)     NOT NULL,
  nickname       VARCHAR(16)     NOT NULL,
  password_hash  VARBINARY(32)   NOT NULL,
  password_salt  VARBINARY(16)   NOT NULL,
  created_at     DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  last_login_at  DATETIME(3)     NULL,
  PRIMARY KEY (account_id),
  UNIQUE KEY uq_accounts_username (username),
  UNIQUE KEY uq_accounts_nickname (nickname)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_as_cs;

-- 2. 접속 및 종료 이력 (감사·통계·장애 원인 추적)
CREATE TABLE IF NOT EXISTS connection_logs (
  log_id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_id        BIGINT UNSIGNED NOT NULL,
  remote_ip         VARCHAR(45)     NOT NULL,
  login_at          DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  logout_at         DATETIME(3)     NULL,
  last_room_id      INT UNSIGNED    NOT NULL DEFAULT 1,
  disconnect_reason VARCHAR(32)     NOT NULL DEFAULT 'CONNECTED',
  PRIMARY KEY (log_id),
  KEY idx_conn_account_login (account_id, login_at DESC),
  CONSTRAINT fk_conn_account FOREIGN KEY (account_id) REFERENCES accounts(account_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_as_cs;
